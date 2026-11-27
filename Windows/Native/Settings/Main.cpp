#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
// GDI+ 需要 IStream/PROPID 声明，WIN32_LEAN_AND_MEAN 下不会被 windows.h 带进来。
#include <objidl.h>
#include <gdiplus.h>
#include <wrl/client.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "Version.h"
#include "resource.h"
#include "StylePreview.h"
#include "../Common/CandidateStyle.h"
#include "../Common/CorrectionPreferences.h"
#include "../Common/Pipe.h"
#include "../Protocol/Protocol.h"

using Microsoft::WRL::ComPtr;

namespace {
constexpr std::size_t kMaxSchemaBytes = 128 * 1024;
constexpr DWORD kDeployTimeoutMs = 10 * 60 * 1000;

struct SchemaInfo {
    std::string id;
    std::wstring name;
    bool fromUserDirectory = false;
};

HINSTANCE g_instance = nullptr;
HWND g_dialog = nullptr;
std::filesystem::path g_installDirectory;
std::filesystem::path g_dataRoot;
std::vector<SchemaInfo> g_schemas;
xmjd::CandidateStyleCatalog g_styles;
std::vector<xmjd::CandidateStyle> g_styleList;
bool g_busy = false;
bool g_selfTest = false;
bool g_syncingStyleCombo = false;
bool g_previewDark = false;

void appendLog(const std::wstring& message);

// Console helpers for the CI entry points; the process is a GUI binary, so
// callers redirect stdout to a file instead of scraping a console.
void printLine(const std::wstring& text) {
    const std::string utf8 = xmjd::win::narrow(text) + "\n";
    std::fwrite(utf8.data(), 1, utf8.size(), stdout);
    std::fflush(stdout);
}

std::filesystem::path hostPath() {
    return g_installDirectory / L"XMJDHost.exe";
}

std::filesystem::path settingsPath() {
    return g_dataRoot / L"settings.ini";
}

std::filesystem::path userConfigPath() {
    return g_dataRoot / L"UserData" / L"Rime" / L"default.custom.yaml";
}

std::filesystem::path deployMarkerPath() {
    return g_dataRoot / L"Staging" / L".xmjd-deploy-pending";
}

std::filesystem::path importedSchemesPath() {
    return g_dataRoot / L"UserData" / L"Rime" / L"imported";
}

std::string trimAscii(std::string_view value) {
    std::size_t begin = 0;
    std::size_t end = value.size();
    while (begin < end && (value[begin] == ' ' || value[begin] == '\t')) ++begin;
    while (end > begin && (value[end - 1] == ' ' || value[end - 1] == '\t' || value[end - 1] == '\r')) --end;
    return std::string(value.substr(begin, end - begin));
}

std::size_t leadingSpaces(std::string_view line) {
    std::size_t count = 0;
    while (count < line.size() && (line[count] == ' ' || line[count] == '\t')) ++count;
    return count;
}

std::string stripQuotes(std::string value) {
    if (value.size() >= 2 &&
        ((value.front() == '"' && value.back() == '"') ||
         (value.front() == '\'' && value.back() == '\''))) {
        return value.substr(1, value.size() - 2);
    }
    return value;
}

bool validSchemaId(std::string_view id) {
    if (id.empty() || id.size() > 64) return false;
    for (const unsigned char byte : id) {
        if (!(std::isalnum(byte) || byte == '.' || byte == '_' || byte == '-')) return false;
    }
    return true;
}

std::optional<std::string> readFile(const std::filesystem::path& path, std::size_t limit) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size > limit) return std::nullopt;
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return std::nullopt;
    std::string value(static_cast<std::size_t>(size), '\0');
    if (!value.empty()) {
        stream.read(value.data(), static_cast<std::streamsize>(value.size()));
        if (!stream || stream.gcount() != static_cast<std::streamsize>(value.size())) return std::nullopt;
    }
    return value;
}

bool writeFile(const std::filesystem::path& path, std::string_view value) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) return false;
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) return false;
    stream.write(value.data(), static_cast<std::streamsize>(value.size()));
    return stream.good();
}

bool writeFileAtomic(const std::filesystem::path& path, std::string_view value) {
    const auto temporary = path.wstring() + L".tmp";
    if (!writeFile(temporary, value)) return false;
    return MoveFileExW(temporary.c_str(), path.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

void splitLines(const std::string& text, std::vector<std::string>& lines) {
    std::size_t start = 0;
    while (start < text.size()) {
        const auto end = text.find('\n', start);
        std::string line = end == std::string::npos ? text.substr(start) : text.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(std::move(line));
        if (end == std::string::npos) break;
        start = end + 1;
    }
}

std::string joinLines(const std::vector<std::string>& lines) {
    std::string result;
    for (const auto& line : lines) {
        result += line;
        result.push_back('\n');
    }
    return result;
}

// settings.ini keeps unrelated keys so a newer build can add more without
// discarding the owner's choices.
std::vector<std::pair<std::string, std::string>> readSettings() {
    std::vector<std::pair<std::string, std::string>> entries;
    const auto contents = readFile(settingsPath(), 64 * 1024);
    if (!contents) return entries;
    std::vector<std::string> lines;
    splitLines(*contents, lines);
    for (const auto& line : lines) {
        const std::string trimmed = trimAscii(line);
        if (trimmed.empty() || trimmed.front() == '#') continue;
        const auto equals = trimmed.find('=');
        if (equals == std::string::npos) continue;
        entries.emplace_back(trimAscii(std::string_view(trimmed).substr(0, equals)),
                             trimAscii(std::string_view(trimmed).substr(equals + 1)));
    }
    return entries;
}

std::string settingsValue(std::string_view key) {
    for (const auto& entry : readSettings()) {
        if (entry.first == key) return entry.second;
    }
    return {};
}

bool updateSettings(const std::vector<std::pair<std::string, std::string>>& updates) {
    std::vector<std::pair<std::string, std::string>> entries = readSettings();
    for (const auto& update : updates) {
        const auto found = std::find_if(entries.begin(), entries.end(),
                                        [&](const auto& entry) { return entry.first == update.first; });
        if (update.second.empty()) {
            if (found != entries.end()) entries.erase(found);
        } else if (found != entries.end()) {
            found->second = update.second;
        } else {
            entries.push_back(update);
        }
    }
    std::string contents;
    for (const auto& entry : entries) {
        if (entry.first.empty()) continue;
        contents += entry.first + "=" + entry.second + "\n";
    }
    return writeFileAtomic(settingsPath(), contents);
}

std::string schemaListBlock(const std::string& selected, std::size_t indent) {
    const std::string pad(indent, ' ');
    std::string block = pad + "schema_list:\n";
    block += pad + "  - schema: " + selected + "\n";
    for (const auto& schema : g_schemas) {
        if (schema.id != selected) block += pad + "  - schema: " + schema.id + "\n";
    }
    return block;
}

bool parseSchemaFile(const std::filesystem::path& file, SchemaInfo& info) {
    auto contents = readFile(file, kMaxSchemaBytes);
    if (!contents) return false;
    if (contents->size() >= 3 && static_cast<unsigned char>((*contents)[0]) == 0xEF &&
        static_cast<unsigned char>((*contents)[1]) == 0xBB &&
        static_cast<unsigned char>((*contents)[2]) == 0xBF) {
        contents->erase(0, 3);
    }
    std::vector<std::string> lines;
    splitLines(*contents, lines);
    bool inSchema = false;
    std::string id;
    std::string name;
    for (const auto& line : lines) {
        const std::string trimmed = trimAscii(line);
        if (trimmed == "schema:") {
            inSchema = true;
            continue;
        }
        if (!inSchema) continue;
        if (!line.empty() && line.front() != ' ' && line.front() != '\t') break;
        if (trimmed.rfind("schema_id:", 0) == 0) {
            id = stripQuotes(trimAscii(trimmed.substr(10)));
        } else if (trimmed.rfind("name:", 0) == 0) {
            name = stripQuotes(trimAscii(trimmed.substr(5)));
        }
    }
    if (!validSchemaId(id)) return false;
    if (name.find("\xE5\x8B\xBF\xE9\x80\x89") != std::string::npos) return false; // 勿选
    info.id = id;
    info.name = name.empty() ? xmjd::win::widen(id) : xmjd::win::widen(name);
    return !info.name.empty();
}

void addSchema(const SchemaInfo& info) {
    const auto existing = std::find_if(g_schemas.begin(), g_schemas.end(),
                                       [&](const auto& schema) { return schema.id == info.id; });
    if (existing == g_schemas.end()) {
        g_schemas.push_back(info);
        return;
    }
    // The Rime user directory shadows the bundled resources.
    if (info.fromUserDirectory) *existing = info;
}

bool endsWithSchemaYaml(const std::wstring& name) {
    constexpr std::wstring_view suffix = L".schema.yaml";
    return name.size() >= suffix.size() &&
           name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0;
}

void scanDirectory(const std::filesystem::path& root, bool recursive, bool fromUserDirectory) {
    std::error_code ec;
    if (!std::filesystem::is_directory(root, ec) || ec) return;
    if (!recursive) {
        for (std::filesystem::directory_iterator it(root, ec), end; it != end && !ec; it.increment(ec)) {
            if (!it->is_regular_file(ec) || ec) continue;
            if (!endsWithSchemaYaml(it->path().filename().wstring())) continue;
            SchemaInfo info;
            info.fromUserDirectory = fromUserDirectory;
            if (parseSchemaFile(it->path(), info)) addSchema(info);
        }
        return;
    }
    std::filesystem::recursive_directory_iterator it(root, std::filesystem::directory_options::skip_permission_denied, ec);
    const std::filesystem::recursive_directory_iterator end;
    for (; it != end && !ec; it.increment(ec)) {
        if (!it->is_regular_file(ec) || ec) continue;
        const auto filename = it->path().filename().wstring();
        if (!endsWithSchemaYaml(filename)) continue;
        if (it->path().wstring().find(L"\\build\\") != std::wstring::npos) continue;
        SchemaInfo info;
        info.fromUserDirectory = fromUserDirectory;
        if (parseSchemaFile(it->path(), info)) addSchema(info);
    }
}

bool loadSchemas() {
    g_schemas.clear();
    const std::string source = settingsValue("scheme_source");
    if (!source.empty()) {
        scanDirectory(xmjd::win::widen(source), false, true);
    } else {
        scanDirectory(g_installDirectory / L"Resources" / L"Rime", false, false);
        scanDirectory(g_dataRoot / L"UserData" / L"Rime", true, true);
    }
    const auto rank = [](const std::string& id) {
        if (id == "xmjd6") return 0;
        if (id == "pinyin_simp") return 1;
        if (id == "liangfen") return 2;
        return 3;
    };
    std::stable_sort(g_schemas.begin(), g_schemas.end(), [&](const SchemaInfo& left, const SchemaInfo& right) {
        const int leftRank = rank(left.id);
        const int rightRank = rank(right.id);
        return leftRank != rightRank ? leftRank < rightRank : left.id < right.id;
    });
    return !g_schemas.empty();
}

std::string readSelectedSchema() {
    const std::string value = settingsValue("schema");
    return validSchemaId(value) ? value : std::string();
}

std::wstring selectedSchemaName(const std::string& id) {
    const auto found = std::find_if(g_schemas.begin(), g_schemas.end(),
                                    [&](const auto& schema) { return schema.id == id; });
    return found == g_schemas.end() ? xmjd::win::widen(id) : found->name;
}

bool replaceSchemaList(std::string& content, const std::string& selected) {
    std::vector<std::string> lines;
    splitLines(content, lines);
    std::size_t index = 0;
    std::size_t indent = 0;
    bool found = false;
    for (; index < lines.size(); ++index) {
        const std::string trimmed = trimAscii(lines[index]);
        if (trimmed.rfind("schema_list:", 0) == 0) {
            indent = leadingSpaces(lines[index]);
            found = true;
            break;
        }
    }
    if (found) {
        std::size_t end = index + 1;
        while (end < lines.size()) {
            if (trimAscii(lines[end]).empty()) {
                ++end;
                continue;
            }
            if (leadingSpaces(lines[end]) <= indent) break;
            ++end;
        }
        std::vector<std::string> block;
        splitLines(schemaListBlock(selected, indent), block);
        if (!block.empty() && block.back().empty()) block.pop_back();
        lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(index),
                    lines.begin() + static_cast<std::ptrdiff_t>(end));
        lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(index), block.begin(), block.end());
    } else {
        std::size_t patchIndex = 0;
        bool hasPatch = false;
        for (; patchIndex < lines.size(); ++patchIndex) {
            if (trimAscii(lines[patchIndex]) == "patch:") {
                hasPatch = true;
                break;
            }
        }
        std::vector<std::string> block;
        splitLines(schemaListBlock(selected, 2), block);
        if (!block.empty() && block.back().empty()) block.pop_back();
        if (hasPatch) {
            lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(patchIndex + 1), block.begin(), block.end());
        } else {
            std::vector<std::string> prefix{"patch:"};
            prefix.insert(prefix.end(), block.begin(), block.end());
            lines.insert(lines.begin(), prefix.begin(), prefix.end());
        }
    }
    content = joinLines(lines);
    return true;
}

bool writeBundledSchemaList(const std::string& selected, std::wstring& error) {
    const auto configPath = userConfigPath();
    std::error_code ec;
    std::filesystem::create_directories(configPath.parent_path(), ec);
    if (ec) {
        error = L"无法创建用户配置目录：" + configPath.parent_path().wstring();
        return false;
    }
    auto existing = readFile(configPath, 1024 * 1024);
    std::string content;
    if (existing) {
        content = *existing;
        const auto backup = configPath.wstring() + L".xmjd-backup";
        if (!std::filesystem::exists(backup, ec)) {
            std::filesystem::copy_file(configPath, backup, std::filesystem::copy_options::none, ec);
            if (ec) {
                error = L"无法备份现有方案配置：" + backup;
                return false;
            }
        }
    } else {
        content = "# Generated by XMJD Settings. Other patches may be added below.\npatch:\n";
        content += schemaListBlock(selected, 2);
        content += "  switcher:\n    hotkeys:\n      - F4\n      - Control+grave\n";
    }
    if (existing && !replaceSchemaList(content, selected)) {
        error = L"无法更新方案列表。";
        return false;
    }
    if (!writeFileAtomic(configPath, content)) {
        error = L"无法写入方案配置：" + configPath.wstring();
        return false;
    }
    return true;
}

bool writeConfiguration(const std::string& selected, std::wstring& error) {
    if (!validSchemaId(selected) ||
        std::none_of(g_schemas.begin(), g_schemas.end(), [&](const auto& schema) { return schema.id == selected; })) {
        error = L"未知的输入方案。";
        return false;
    }
    if (settingsValue("scheme_source").empty() && !writeBundledSchemaList(selected, error)) return false;
    if (!updateSettings({{"schema", selected}})) {
        error = L"无法写入设置文件：" + settingsPath().wstring();
        return false;
    }
    if (!writeFileAtomic(deployMarkerPath(), "pending\n")) {
        error = L"无法写入部署标记：" + deployMarkerPath().wstring();
        return false;
    }
    appendLog(L"apply schema=" + xmjd::win::widen(selected));
    return true;
}

void appendLog(const std::wstring& message) {
    std::error_code ec;
    const auto directory = g_dataRoot / L"Logs";
    std::filesystem::create_directories(directory, ec);
    if (ec) return;
    const auto path = directory / L"settings.log";
    const auto size = std::filesystem::file_size(path, ec);
    if (!ec && size > 256 * 1024) std::filesystem::remove(path, ec);
    SYSTEMTIME time{};
    GetLocalTime(&time);
    wchar_t stamp[64]{};
    swprintf_s(stamp, L"%04u-%02u-%02u %02u:%02u:%02u ",
               static_cast<unsigned>(time.wYear), static_cast<unsigned>(time.wMonth),
               static_cast<unsigned>(time.wDay), static_cast<unsigned>(time.wHour),
               static_cast<unsigned>(time.wMinute), static_cast<unsigned>(time.wSecond));
    const std::string line = xmjd::win::narrow(std::wstring(stamp) + message + L"\r\n");
    std::ofstream stream(path, std::ios::binary | std::ios::app);
    if (stream) stream.write(line.data(), static_cast<std::streamsize>(line.size()));
}

bool isElevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION elevation{};
    DWORD returned = 0;
    const bool elevated = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &returned) &&
                          elevation.TokenIsElevated;
    CloseHandle(token);
    return elevated;
}

enum class HostState { Unavailable, Ready, Error };

HostState queryHealth(std::wstring& detail) {
    xmjd::win::PipeClient client;
    if (!client.connect(hostPath())) {
        detail = L"引擎未运行";
        return HostState::Unavailable;
    }
    xmjd::Request request;
    request.operation = xmjd::Operation::Health;
    request.id.connectionEpoch = client.epoch();
    request.id.sequence = 1;
    const auto response = client.transact(request, 1000, true);
    if (!response) {
        detail = L"引擎连接异常";
        return HostState::Error;
    }
    if (response->status == xmjd::Status::Ok && response->handled) {
        detail = L"引擎已就绪";
        return HostState::Ready;
    }
    detail = L"引擎尚未就绪（" + xmjd::win::widen(xmjd::statusName(response->status)) + L"）";
    return HostState::Error;
}

bool startHost() {
    const auto executable = hostPath();
    std::wstring commandLine = L"\"" + executable.wstring() + L"\"";
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.c_str(), commandLine.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, g_installDirectory.c_str(), &startup, &process)) {
        return false;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

void pumpMessages() {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        if (g_dialog && IsDialogMessageW(g_dialog, &message)) continue;
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

bool waitForHealth(DWORD timeoutMs, std::wstring& detail) {
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    for (;;) {
        if (queryHealth(detail) == HostState::Ready) return true;
        if (GetTickCount64() >= deadline) return false;
        pumpMessages();
        Sleep(200);
    }
}

bool deployNow(std::wstring& error) {
    xmjd::win::PipeClient client;
    if (!client.connect(hostPath())) {
        error = L"引擎未运行，无法立即部署。";
        return false;
    }
    xmjd::Request request;
    request.operation = xmjd::Operation::Deploy;
    request.id.connectionEpoch = client.epoch();
    request.id.sequence = 1;
    const ULONGLONG started = GetTickCount64();
    const auto response = client.transact(request, kDeployTimeoutMs, true);
    const auto elapsed = GetTickCount64() - started;
    if (!response) {
        error = L"未收到有效的部署结果（用时 " + std::to_wstring(elapsed / 1000) +
                L" 秒），暂不能确认是否完成。请先检查引擎状态和日志。";
        appendLog(L"deploy transport failure elapsed_ms=" + std::to_wstring(elapsed));
        return false;
    }
    if (response->status == xmjd::Status::Ok && response->handled) {
        appendLog(L"deploy ok elapsed_ms=" + std::to_wstring(elapsed));
        return true;
    }
    const auto code = response->diagnosticCode;
    error = L"部署失败（" + xmjd::win::widen(xmjd::statusName(response->status)) +
            L"，错误码 " + std::to_wstring(code) + L"）。请查看日志后重试。";
    appendLog(L"deploy failed status=" + xmjd::win::widen(xmjd::statusName(response->status)) +
              L" code=" + std::to_wstring(code) + L" elapsed_ms=" + std::to_wstring(elapsed));
    return false;
}

bool ensureDeployed(std::wstring& error) {
    std::wstring detail;
    const HostState state = queryHealth(detail);
    if (state == HostState::Ready) {
        if (!deployNow(error)) return false;
        if (!waitForHealth(15000, detail)) {
            error = L"部署已完成，但引擎没有恢复响应：" + detail;
            return false;
        }
        return true;
    }
    if (!startHost()) {
        if (!waitForHealth(60000, detail)) {
            error = L"无法启动 XMJDHost.exe。请确认以普通用户身份运行，并检查程序目录。";
            appendLog(L"start host failed: " + detail);
            return false;
        }
        return true;
    }
    if (!waitForHealth(180000, detail)) {
        error = L"引擎启动或部署超时。请查看 " + (g_dataRoot / L"Logs").wstring() + L" 后重试。";
        appendLog(L"host health timeout: " + detail);
        return false;
    }
    return true;
}

// MARK: - Scheme source

bool isSubPath(const std::filesystem::path& parent, const std::filesystem::path& child) {
    std::wstring parentText = parent.lexically_normal().wstring();
    std::wstring childText = child.lexically_normal().wstring();
    const auto lower = [](wchar_t character) { return static_cast<wchar_t>(towlower(character)); };
    std::transform(parentText.begin(), parentText.end(), parentText.begin(), lower);
    std::transform(childText.begin(), childText.end(), childText.begin(), lower);
    while (!parentText.empty() && (parentText.back() == L'\\' || parentText.back() == L'/')) parentText.pop_back();
    while (!childText.empty() && (childText.back() == L'\\' || childText.back() == L'/')) childText.pop_back();
    if (parentText.empty() || childText.empty()) return false;
    if (childText == parentText) return true;
    return childText.size() > parentText.size() &&
           childText.compare(0, parentText.size(), parentText) == 0 &&
           (childText[parentText.size()] == L'\\' || childText[parentText.size()] == L'/');
}

std::wstring sanitizeFolderName(std::wstring name) {
    for (auto& character : name) {
        if (character == L'\\' || character == L'/' || character == L':' || character == L'*' ||
            character == L'?' || character == L'"' || character == L'<' || character == L'>' ||
            character == L'|') {
            character = L'-';
        }
    }
    while (!name.empty() && (name.back() == L'.' || name.back() == L' ')) name.pop_back();
    if (name.empty()) name = L"scheme";
    if (name.size() > 48) name.resize(48);
    return name;
}

bool setSchemeSource(const std::filesystem::path& source, std::wstring& error, std::size_t& schemas) {
    schemas = 0;
    std::error_code ec;
    const auto resolved = std::filesystem::canonical(source, ec);
    if (ec || !std::filesystem::is_regular_file(resolved / L"default.yaml", ec)) {
        error = L"请选择根层包含 default.yaml 和 *.schema.yaml 的 Rime 目录，而不是 build 子目录。";
        return false;
    }
    const auto owned = std::filesystem::weakly_canonical(g_dataRoot, ec);
    if (ec || isSubPath(owned, resolved) || isSubPath(resolved, owned)) {
        error = L"方案来源必须和星猫的运行目录分开。";
        return false;
    }
    for (std::filesystem::directory_iterator it(resolved, ec), end; it != end && !ec; it.increment(ec)) {
        if (it->is_regular_file(ec) && endsWithSchemaYaml(it->path().filename().wstring())) ++schemas;
        if (ec) break;
    }
    if (ec || !schemas) {
        error = L"无法读取所选目录，或根层没有 .schema.yaml。";
        return false;
    }
    if (!updateSettings({{"scheme_source", xmjd::win::narrow(resolved.wstring())}})) {
        error = L"无法保存方案来源。";
        return false;
    }
    return true;
}

bool removeImportedSchemes(std::wstring& error) {
    std::error_code ec;
    const auto directory = importedSchemesPath();
    if (std::filesystem::exists(directory, ec)) {
        std::filesystem::remove_all(directory, ec);
        if (ec) {
            error = L"无法删除已导入的方案：" + directory.wstring() +
                    L"\n请关闭正在使用这些方案的应用后重试。";
            return false;
        }
    }
    if (!updateSettings({{"scheme_source", ""}, {"schema", "xmjd6"}})) {
        error = L"无法清除方案来源设置。";
        return false;
    }
    appendLog(L"remove imported schemes");
    return true;
}

std::wstring abbreviatePath(const std::wstring& path) {
    if (path.size() <= 68) return path;
    return L"…" + path.substr(path.size() - 66);
}

void refreshSourceLabel() {
    if (!g_dialog) return;
    const std::string source = settingsValue("scheme_source");
    std::wstring text;
    if (source.empty()) {
        text = L"引擎使用：内置方案（程序目录 Resources\\Rime）";
    } else {
        text = L"引擎使用：" + abbreviatePath(xmjd::win::widen(source));
        std::error_code ec;
        if (!std::filesystem::is_directory(xmjd::win::widen(source), ec)) {
            text += L"（目录已不存在，已回退到内置方案）";
        } else {
            text += L"；该目录只读，星猫不写入";
        }
    }
    SetDlgItemTextW(g_dialog, IDC_SOURCE_LABEL, text.c_str());
}

std::optional<std::filesystem::path> pickFolder(const std::wstring& title) {
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dialog.GetAddressOf())))) {
        return std::nullopt;
    }
    DWORD options = 0;
    if (FAILED(dialog->GetOptions(&options))) return std::nullopt;
    dialog->SetOptions(static_cast<FILEOPENDIALOGOPTIONS>(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM |
                                                          FOS_PATHMUSTEXIST));
    dialog->SetTitle(title.c_str());
    if (dialog->Show(g_dialog) != S_OK) return std::nullopt;
    ComPtr<IShellItem> item;
    if (FAILED(dialog->GetResult(item.GetAddressOf()))) return std::nullopt;
    PWSTR path = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) || !path) return std::nullopt;
    std::filesystem::path result(path);
    CoTaskMemFree(path);
    return result;
}

std::optional<std::filesystem::path> pickFile(const std::wstring& title, const wchar_t* filter) {
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dialog.GetAddressOf())))) {
        return std::nullopt;
    }
    COMDLG_FILTERSPEC specs[] = {{L"星猫候选皮肤", filter}};
    dialog->SetFileTypes(1, specs);
    dialog->SetTitle(title.c_str());
    if (dialog->Show(g_dialog) != S_OK) return std::nullopt;
    ComPtr<IShellItem> item;
    if (FAILED(dialog->GetResult(item.GetAddressOf()))) return std::nullopt;
    PWSTR path = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) || !path) return std::nullopt;
    std::filesystem::path result(path);
    CoTaskMemFree(path);
    return result;
}

std::optional<std::filesystem::path> askSavePath(const std::wstring& title, const std::wstring& suggested) {
    ComPtr<IFileSaveDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dialog.GetAddressOf())))) {
        return std::nullopt;
    }
    COMDLG_FILTERSPEC specs[] = {{L"星猫候选皮肤", L"*.json"}};
    dialog->SetFileTypes(1, specs);
    dialog->SetDefaultExtension(L"json");
    dialog->SetTitle(title.c_str());
    dialog->SetFileName(suggested.c_str());
    if (dialog->Show(g_dialog) != S_OK) return std::nullopt;
    ComPtr<IShellItem> item;
    if (FAILED(dialog->GetResult(item.GetAddressOf()))) return std::nullopt;
    PWSTR path = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) || !path) return std::nullopt;
    std::filesystem::path result(path);
    CoTaskMemFree(path);
    return result;
}

// MARK: - 候选皮肤 / 颜色槽位

constexpr int kColourControls[] = {IDC_COLOR_BG, IDC_COLOR_TEXT, IDC_COLOR_SELECTED_TEXT,
                                   IDC_COLOR_SECONDARY, IDC_COLOR_HIGHLIGHT_START,
                                   IDC_COLOR_HIGHLIGHT_END, IDC_COLOR_BG_END};

bool isColourControl(int id) {
    return std::find(std::begin(kColourControls), std::end(kColourControls), id) !=
           std::end(kColourControls);
}

xmjd::CandidateDecoration& ensureDecoration(xmjd::CandidateStyle& style) {
    if (!style.decoration) {
        // 没有装饰配色（如「经典竖排」）时按主色补一套，避免打开取色器就写出全黑。
        const xmjd::ColorPair white{{1, 1, 1, 1}, {1, 1, 1, 1}};
        style.decoration = xmjd::CandidateDecoration{style.background, style.accent, style.accent,
                                                     white, style.text};
    }
    return *style.decoration;
}

// 每个色板对应一个配色槽；`dark` 由「深色预览」勾选状态决定，编辑哪套配色就预览哪套。
xmjd::StyleColor* colourSlot(xmjd::CandidateStyle& style, int id, bool dark) {
    switch (id) {
    case IDC_COLOR_BG:
        return dark ? &style.background.dark : &style.background.light;
    case IDC_COLOR_TEXT:
        return dark ? &style.text.dark : &style.text.light;
    case IDC_COLOR_SELECTED_TEXT: {
        auto& decoration = ensureDecoration(style);
        return dark ? &decoration.selectedText.dark : &decoration.selectedText.light;
    }
    case IDC_COLOR_SECONDARY: {
        auto& decoration = ensureDecoration(style);
        return dark ? &decoration.secondaryText.dark : &decoration.secondaryText.light;
    }
    case IDC_COLOR_HIGHLIGHT_START: {
        auto& decoration = ensureDecoration(style);
        return dark ? &decoration.highlightMiddle.dark : &decoration.highlightMiddle.light;
    }
    case IDC_COLOR_HIGHLIGHT_END: {
        auto& decoration = ensureDecoration(style);
        return dark ? &decoration.highlightEnd.dark : &decoration.highlightEnd.light;
    }
    case IDC_COLOR_BG_END: {
        auto& decoration = ensureDecoration(style);
        return dark ? &decoration.backgroundEnd.dark : &decoration.backgroundEnd.light;
    }
    default:
        return nullptr;
    }
}

COLORREF styleColour(const xmjd::StyleColor& colour) {
    const auto channel = [](double value) {
        return static_cast<BYTE>(std::lround(std::clamp(value, 0.0, 1.0) * 255));
    };
    return RGB(channel(colour.red), channel(colour.green), channel(colour.blue));
}

// MARK: - Candidate styles

void refreshStyleEditors();
void repaintStylePreview();

void refreshStyleList() {
    if (!g_dialog) return;
    g_styles = xmjd::loadCandidateStyleCatalog(g_dataRoot);
    g_styleList.clear();
    g_styleList.insert(g_styleList.end(), g_styles.builtin.begin(), g_styles.builtin.end());
    g_styleList.insert(g_styleList.end(), g_styles.custom.begin(), g_styles.custom.end());
    g_syncingStyleCombo = true;
    SendDlgItemMessageW(g_dialog, IDC_STYLE_LIST, CB_RESETCONTENT, 0, 0);
    int selection = 0;
    for (std::size_t index = 0; index < g_styleList.size(); ++index) {
        const std::wstring label = xmjd::win::widen(g_styleList[index].name) + L"  (" +
                                   (g_styleList[index].horizontal ? L"横排" : L"竖排") + L")";
        SendDlgItemMessageW(g_dialog, IDC_STYLE_LIST, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
        if (g_styleList[index].id == g_styles.active.id) selection = static_cast<int>(index);
    }
    if (!g_styleList.empty()) {
        SendDlgItemMessageW(g_dialog, IDC_STYLE_LIST, CB_SETCURSEL, static_cast<WPARAM>(selection), 0);
    }
    g_syncingStyleCombo = false;
    const xmjd::CandidateStyle& active = g_styles.active;
    // 这一行只有一行字的宽度，多写就会被截断；详细参数在右侧控件里都看得到。
    SetDlgItemTextW(g_dialog, IDC_STYLE_DETAIL, (L"当前：" + xmjd::win::widen(active.name)).c_str());
    refreshStyleEditors();
}

bool applyStyleByIndex(int index) {
    if (index < 0 || static_cast<std::size_t>(index) >= g_styleList.size()) return false;
    std::string error;
    if (!xmjd::activateCandidateStyle(g_dataRoot, g_styleList[static_cast<std::size_t>(index)], error)) {
        MessageBoxW(g_dialog, xmjd::win::widen(error).c_str(), L"星猫键道设置", MB_OK | MB_ICONERROR);
        return false;
    }
    appendLog(L"activate style=" + xmjd::win::widen(g_styleList[static_cast<std::size_t>(index)].id));
    g_styles.active = g_styleList[static_cast<std::size_t>(index)];
    updateSettings({{"style", g_styles.active.id}});
    SetDlgItemTextW(g_dialog, IDC_STYLE_DETAIL, (L"已启用：" + xmjd::win::widen(g_styles.active.name)).c_str());
    refreshStyleEditors();
    return true;
}

void repaintStylePreview() {
    if (!g_dialog) return;
    if (HWND preview = GetDlgItem(g_dialog, IDC_STYLE_PREVIEW)) InvalidateRect(preview, nullptr, TRUE);
}

void refreshStyleEditors() {
    if (!g_dialog) return;
    g_syncingStyleCombo = true;
    const xmjd::CandidateStyle& style = g_styles.active;

    HWND fontCombo = GetDlgItem(g_dialog, IDC_STYLE_FONT);
    if (fontCombo) {
        SendMessageW(fontCombo, CB_RESETCONTENT, 0, 0);
        SendMessageW(fontCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"跟随系统"));
        const wchar_t* faces[] = {L"Microsoft YaHei UI", L"微软雅黑", L"等线",
                                  L"黑体", L"宋体", L"Segoe UI", L"PingFang SC"};
        int selection = 0;
        const std::wstring current = xmjd::win::widen(style.fontName);
        for (const wchar_t* face : faces) {
            const int index = static_cast<int>(SendMessageW(fontCombo, CB_ADDSTRING, 0,
                                                            reinterpret_cast<LPARAM>(face)));
            if (!current.empty() && lstrcmpiW(current.c_str(), face) == 0) selection = index;
        }
        if (!current.empty() && selection == 0) {
            // 导入皮肤可能用了列表外的字体，补一项，避免误显示为「跟随系统」。
            const int index = static_cast<int>(SendMessageW(fontCombo, CB_ADDSTRING, 0,
                                                            reinterpret_cast<LPARAM>(current.c_str())));
            if (index >= 0) selection = index;
        }
        SendMessageW(fontCombo, CB_SETCURSEL, static_cast<WPARAM>(selection), 0);
    }

    HWND layoutCombo = GetDlgItem(g_dialog, IDC_STYLE_LAYOUT);
    if (layoutCombo) {
        SendMessageW(layoutCombo, CB_RESETCONTENT, 0, 0);
        SendMessageW(layoutCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"横排"));
        SendMessageW(layoutCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"竖排"));
        SendMessageW(layoutCombo, CB_SETCURSEL, style.horizontal ? 0 : 1, 0);
    }

    SetDlgItemInt(g_dialog, IDC_STYLE_SIZE, static_cast<UINT>(std::lround(style.fontSize)), FALSE);
    SetDlgItemInt(g_dialog, IDC_STYLE_ROW, static_cast<UINT>(std::lround(style.rowHeight)), FALSE);
    SetDlgItemInt(g_dialog, IDC_STYLE_RADIUS, static_cast<UINT>(std::lround(style.cornerRadius)), FALSE);
    SetDlgItemInt(g_dialog, IDC_STYLE_OPACITY, static_cast<UINT>(std::lround(style.opacity * 100)), FALSE);
    SetDlgItemInt(g_dialog, IDC_STYLE_MAXWIDTH, static_cast<UINT>(std::lround(style.maxWidth)), FALSE);
    SetDlgItemInt(g_dialog, IDC_STYLE_COMMENT_SIZE, static_cast<UINT>(std::lround(style.commentSize)), FALSE);
    SetDlgItemInt(g_dialog, IDC_STYLE_PADDING, static_cast<UINT>(std::lround(style.padding)), FALSE);
    SetDlgItemInt(g_dialog, IDC_STYLE_ITEM_SPACING, static_cast<UINT>(std::lround(style.itemSpacing)), FALSE);
    SetDlgItemInt(g_dialog, IDC_STYLE_PAD_H, static_cast<UINT>(std::lround(style.horizontalPadding())), FALSE);
    SetDlgItemInt(g_dialog, IDC_STYLE_PAD_V, static_cast<UINT>(std::lround(style.verticalPadding())), FALSE);
    SetDlgItemInt(g_dialog, IDC_STYLE_ITEM_RADIUS, static_cast<UINT>(std::lround(style.itemRadius)), FALSE);
    CheckDlgButton(g_dialog, IDC_STYLE_LABELS, style.showLabels ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(g_dialog, IDC_STYLE_COMMENTS, style.showComments ? BST_CHECKED : BST_UNCHECKED);
    for (const int id : kColourControls) {
        if (HWND swatch = GetDlgItem(g_dialog, id)) InvalidateRect(swatch, nullptr, TRUE);
    }
    g_syncingStyleCombo = false;
    repaintStylePreview();
}

bool saveActiveStyleEdit() {
    if (!g_dialog) return false;
    std::string error;
    if (!xmjd::activateCandidateStyle(g_dataRoot, g_styles.active, error)) {
        SetDlgItemTextW(g_dialog, IDC_STYLE_DETAIL, (L"保存失败：" + xmjd::win::widen(error)).c_str());
        return false;
    }
    updateSettings({{"style", g_styles.active.id}});
    const xmjd::CandidateStyle& active = g_styles.active;
    SetDlgItemTextW(g_dialog, IDC_STYLE_DETAIL, (L"已保存：" + xmjd::win::widen(active.name)).c_str());
    return true;
}

void applyStyleEdits() {
    if (!g_dialog || g_syncingStyleCombo || g_busy) return;
    xmjd::CandidateStyle& style = g_styles.active;
    BOOL ok = FALSE;
    UINT value = GetDlgItemInt(g_dialog, IDC_STYLE_SIZE, &ok, FALSE);
    if (ok) style.fontSize = std::clamp<int>(static_cast<int>(value), 10, 48);
    value = GetDlgItemInt(g_dialog, IDC_STYLE_ROW, &ok, FALSE);
    if (ok) style.rowHeight = std::clamp<int>(static_cast<int>(value), 20, 96);
    value = GetDlgItemInt(g_dialog, IDC_STYLE_RADIUS, &ok, FALSE);
    if (ok) style.cornerRadius = std::clamp<int>(static_cast<int>(value), 0, 30);
    value = GetDlgItemInt(g_dialog, IDC_STYLE_OPACITY, &ok, FALSE);
    if (ok) style.opacity = std::clamp<int>(static_cast<int>(value), 30, 100) / 100.0;
    value = GetDlgItemInt(g_dialog, IDC_STYLE_MAXWIDTH, &ok, FALSE);
    if (ok) style.maxWidth = std::clamp<int>(static_cast<int>(value), 180, 1000);
    value = GetDlgItemInt(g_dialog, IDC_STYLE_COMMENT_SIZE, &ok, FALSE);
    // 取值区间必须和 validCandidateStyle 一致，否则写盘会以"保存失败"收场。
    if (ok) style.commentSize = std::clamp<int>(static_cast<int>(value), 10, 48);
    value = GetDlgItemInt(g_dialog, IDC_STYLE_PADDING, &ok, FALSE);
    if (ok) style.padding = std::clamp<int>(static_cast<int>(value), 0, 48);
    value = GetDlgItemInt(g_dialog, IDC_STYLE_ITEM_SPACING, &ok, FALSE);
    if (ok) {
        style.itemSpacing = std::clamp<int>(static_cast<int>(value), 0, 48);
        // 项距是横竖两向的兜底值，改了就把单独覆盖清掉，否则界面上改了没反应。
        style.spacingHorizontal.reset();
        style.spacingVertical.reset();
    }
    value = GetDlgItemInt(g_dialog, IDC_STYLE_PAD_H, &ok, FALSE);
    if (ok) style.paddingHorizontal = std::clamp<int>(static_cast<int>(value), 0, 48);
    value = GetDlgItemInt(g_dialog, IDC_STYLE_PAD_V, &ok, FALSE);
    if (ok) style.paddingVertical = std::clamp<int>(static_cast<int>(value), 0, 48);
    value = GetDlgItemInt(g_dialog, IDC_STYLE_ITEM_RADIUS, &ok, FALSE);
    if (ok) style.itemRadius = std::clamp<int>(static_cast<int>(value), 0, 48);

    const int fontIndex = static_cast<int>(SendDlgItemMessageW(g_dialog, IDC_STYLE_FONT, CB_GETCURSEL, 0, 0));
    if (fontIndex == 0) {
        style.fontName.clear();
    } else if (fontIndex > 0) {
        wchar_t face[160]{};
        SendDlgItemMessageW(g_dialog, IDC_STYLE_FONT, CB_GETLBTEXT, fontIndex,
                            reinterpret_cast<LPARAM>(face));
        style.fontName = xmjd::win::narrow(face);
    }

    const int layoutIndex = static_cast<int>(SendDlgItemMessageW(g_dialog, IDC_STYLE_LAYOUT, CB_GETCURSEL, 0, 0));
    if (layoutIndex >= 0) style.horizontal = layoutIndex == 0;
    style.showLabels = IsDlgButtonChecked(g_dialog, IDC_STYLE_LABELS) == BST_CHECKED;
    style.showComments = IsDlgButtonChecked(g_dialog, IDC_STYLE_COMMENTS) == BST_CHECKED;

    if (!saveActiveStyleEdit()) return;
    repaintStylePreview();
}

void resetActiveStyle() {
    const auto& builtins = xmjd::builtinCandidateStyles();
    const auto found = std::find_if(builtins.begin(), builtins.end(),
        [&](const xmjd::CandidateStyle& preset) { return preset.id == g_styles.active.id; });
    g_styles.active = found != builtins.end() ? *found : builtins.front();
    if (saveActiveStyleEdit()) refreshStyleEditors();
}

bool importStyleFile(const std::filesystem::path& file, std::wstring& error) {
    const auto contents = readFile(file, 256 * 1024);
    if (!contents) {
        error = L"无法读取皮肤文件（上限 256 KiB）。";
        return false;
    }
    xmjd::CandidateStyle style;
    std::string parseError;
    if (!xmjd::parseCandidateStyle(*contents, style, parseError)) {
        error = xmjd::win::widen(parseError);
        return false;
    }
    std::wstring id = xmjd::win::widen(style.id);
    style.id = xmjd::win::narrow(sanitizeFolderName(id));
    if (style.id.empty()) style.id = "custom";
    style.id += "-" + std::to_string(GetTickCount64() % 100000);
    if (style.id.size() > 90) style.id.resize(90);
    const auto target = xmjd::candidateStyleDirectory(g_dataRoot) / (xmjd::win::widen(style.id) + L".json");
    if (!xmjd::writeCandidateStyleFile(style, target, parseError)) {
        error = xmjd::win::widen(parseError);
        return false;
    }
    return true;
}

// MARK: - UI

void setStatus(const std::wstring& text) {
    if (g_dialog) SetDlgItemTextW(g_dialog, IDC_STATUS, text.c_str());
}

void setBusy(bool busy) {
    g_busy = busy;
    for (const int id : {IDC_APPLY, IDC_REDEPLOY, IDC_START_HOST, IDC_OPEN_USER, IDC_OPEN_LOG, IDC_OPEN_LANGUAGE,
                         IDC_CHOOSE_SOURCE, IDC_OPEN_SOURCE, IDC_REMOVE_SOURCE, IDC_STYLE_LIST, IDC_IMPORT_STYLE,
                         IDC_EXPORT_STYLE, IDC_OPEN_STYLES}) {
        EnableWindow(GetDlgItem(g_dialog, id), !busy);
    }
    if (!busy && isElevated()) {
        for (const int id : {IDC_APPLY, IDC_REDEPLOY, IDC_START_HOST, IDC_CHOOSE_SOURCE, IDC_REMOVE_SOURCE,
                             IDC_STYLE_LIST, IDC_IMPORT_STYLE}) {
            EnableWindow(GetDlgItem(g_dialog, id), FALSE);
        }
    }
}

void refreshStatus() {
    std::wstring detail;
    const HostState state = queryHealth(detail);
    if (state == HostState::Ready) {
        const std::string selected = readSelectedSchema();
        const std::wstring name = selected.empty() ? L"星猫键道" : selectedSchemaName(selected);
        setStatus(L"引擎状态：已就绪，当前方案：" + name);
    } else {
        setStatus(L"引擎状态：" + detail + L"（首次输入或点击“启动引擎”时自动启动）");
    }
}

void showError(const std::wstring& text) {
    MessageBoxW(g_dialog, text.c_str(), L"星猫键道设置", MB_OK | MB_ICONERROR);
}

void refreshSchemaList(int preferredIndex) {
    SendDlgItemMessageW(g_dialog, IDC_SCHEMA_LIST, LB_RESETCONTENT, 0, 0);
    for (const auto& schema : g_schemas) {
        const std::wstring label = schema.name + L"  (" + xmjd::win::widen(schema.id) + L")";
        SendDlgItemMessageW(g_dialog, IDC_SCHEMA_LIST, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
    }
    const std::string selected = readSelectedSchema();
    int index = preferredIndex;
    if (index < 0) {
        index = 0;
        for (std::size_t position = 0; position < g_schemas.size(); ++position) {
            if (g_schemas[position].id == selected) index = static_cast<int>(position);
        }
    }
    if (!g_schemas.empty()) {
        SendDlgItemMessageW(g_dialog, IDC_SCHEMA_LIST, LB_SETCURSEL, static_cast<WPARAM>(index), 0);
    }
}

void applySelection() {
    if (g_busy) return;
    const int index = static_cast<int>(SendDlgItemMessageW(g_dialog, IDC_SCHEMA_LIST, LB_GETCURSEL, 0, 0));
    if (index < 0 || static_cast<std::size_t>(index) >= g_schemas.size()) {
        showError(L"请先选择一个输入方案。");
        return;
    }
    const std::string schema = g_schemas[static_cast<std::size_t>(index)].id;
    std::wstring error;
    if (!writeConfiguration(schema, error)) {
        showError(error);
        return;
    }
    setBusy(true);
    setStatus(L"正在部署，可能需要一分钟，请稍候…");
    UpdateWindow(g_dialog);
    if (!ensureDeployed(error)) {
        setBusy(false);
        refreshStatus();
        showError(error);
        return;
    }
    setBusy(false);
    refreshStatus();
    MessageBoxW(g_dialog, L"方案已应用并完成部署。\n\n请回到目标应用重新输入；如果仍显示旧方案，切换一次窗口或输入法再试。",
                L"星猫键道设置", MB_OK | MB_ICONINFORMATION);
}

void redeployOnly() {
    if (g_busy) return;
    setBusy(true);
    setStatus(L"正在重新部署，请稍候…");
    UpdateWindow(g_dialog);
    std::wstring error;
    const bool ok = ensureDeployed(error);
    setBusy(false);
    refreshStatus();
    if (!ok) showError(error);
    else MessageBoxW(g_dialog, L"部署已完成。", L"星猫键道设置", MB_OK | MB_ICONINFORMATION);
}

void startEngine() {
    if (g_busy) return;
    setBusy(true);
    setStatus(L"正在启动引擎…");
    UpdateWindow(g_dialog);
    std::wstring detail;
    if (queryHealth(detail) != HostState::Ready) startHost();
    const bool ready = waitForHealth(30000, detail);
    setBusy(false);
    refreshStatus();
    if (!ready) {
        showError(L"引擎未能启动：" + detail + L"\n\n请以普通用户身份运行设置程序，并确认程序目录完整。");
    }
}

// 换方案来源后必须重启引擎：shared_data_dir 只在 initialize 时读取。
bool restartHostForSourceChange(std::wstring& detail) {
    xmjd::win::PipeClient client;
    if (!client.connect(hostPath())) {
        detail = L"引擎本来就没在运行";
        return true;
    }
    xmjd::Request request;
    request.operation = xmjd::Operation::Shutdown;
    request.id.connectionEpoch = client.epoch();
    request.id.sequence = 1;
    const auto response = client.transact(request, 5000, true);
    if (!response || response->status != xmjd::Status::Ok) {
        detail = L"引擎没有接受重启请求（" +
                 xmjd::win::widen(xmjd::statusName(response ? response->status : xmjd::Status::Unavailable)) +
                 L"），下次登录后才会换目录。";
        appendLog(L"shutdown request failed");
        return false;
    }
    for (int waited = 0; waited < 8000; waited += 100) {
        std::wstring probe;
        if (queryHealth(probe) == HostState::Unavailable) {
            detail = L"已重启";
            return true;
        }
        Sleep(100);
    }
    detail = L"引擎 8 秒内没有退出，重启后再试。";
    return false;
}

void chooseSchemeSource() {
    if (g_busy) return;
    const auto folder = pickFolder(L"选择 Rime 方案目录（根层要有 default.yaml 和 *.schema.yaml）");
    if (!folder) return;
    const auto oldSource = settingsValue("scheme_source");
    std::size_t schemas = 0;
    std::wstring error;
    if (!setSchemeSource(*folder, error, schemas)) {
        showError(error);
        return;
    }
    const auto resolved = xmjd::win::widen(settingsValue("scheme_source"));
    loadSchemas();
    refreshSchemaList(-1);
    refreshSourceLabel();
    // Rime 的 shared_data_dir 只在初始化时生效，必须重启引擎才会真的换目录。
    std::wstring stopped;
    const bool restarted = restartHostForSourceChange(stopped);
    appendLog(L"scheme source = " + resolved + L"，方案 " + std::to_wstring(schemas));
    if (!restarted) {
        MessageBoxW(g_dialog,
                    (L"方案来源已记录，但引擎没重启成功：" + stopped +
                     L"\n\n下次登录后会按这个目录启动。").c_str(),
                    L"星猫键道设置", MB_OK | MB_ICONWARNING);
        return;
    }
    // 起一次引擎确认真的能编译。编译不过就自动回退，不能让人卡在"输入法不工作"。
    startHost();
    std::wstring health;
    if (!waitForHealth(150000, health)) {
        updateSettings({{"scheme_source", oldSource}});
        std::wstring ignored;
        restartHostForSourceChange(ignored);
        loadSchemas();
        refreshSchemaList(-1);
        refreshSourceLabel();
        appendLog(L"scheme source rejected: " + resolved);
        showError(L"这个目录编译失败，已恢复上一个方案来源。\n\n" + health +
                  L"\n\n常见原因：目录里缺 default.yaml 依赖的词典、或方案本身有语法错误。");
        return;
    }
    MessageBoxW(g_dialog,
                (L"方案来源已设为：\n" + resolved + L"\n\n读到 " + std::to_wstring(schemas) +
                 L" 个方案，引擎已按这个目录编译完成。"
                 L"\n\n以后这个目录里改词典，重新部署一次就生效；星猫只读，不往里面写文件。")
                    .c_str(),
                L"星猫键道设置", MB_OK | MB_ICONINFORMATION);
}

void openSchemeSource() {
    const std::string source = settingsValue("scheme_source");
    if (!source.empty()) {
        std::error_code ec;
        const auto path = xmjd::win::widen(source);
        if (std::filesystem::is_directory(path, ec)) {
            ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            return;
        }
    }
    ShellExecuteW(nullptr, L"open", (g_installDirectory / L"Resources" / L"Rime").c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void removeSchemeSource() {
    if (g_busy) return;
    if (MessageBoxW(g_dialog,
                    L"改回内置方案目录？\n\n你选的那个目录不会被改动或删除；"
                    L"用户词库和皮肤设置都保留。引擎会重启并重新部署内置方案。",
                    L"星猫键道设置", MB_YESNO | MB_ICONQUESTION) != IDYES) {
        return;
    }
    if (!updateSettings({{"scheme_source", ""}, {"schema", "xmjd6"}})) {
        showError(L"无法保存内置方案设置。");
        return;
    }
    std::wstring stopped;
    const bool restarted = restartHostForSourceChange(stopped);
    std::error_code cleanEc;
    std::filesystem::remove_all(importedSchemesPath(), cleanEc);
    loadSchemas();
    refreshSchemaList(-1);
    refreshSourceLabel();
    MessageBoxW(g_dialog,
                (L"已改回内置方案目录。" + (restarted ? L"引擎已重启。" : stopped)).c_str(),
                L"星猫键道设置", MB_OK | MB_ICONINFORMATION);
    return;
    std::wstring error;
    if (!removeImportedSchemes(error)) {
        showError(error);
        return;
    }
    loadSchemas();
    refreshSchemaList(-1);
    refreshSourceLabel();
    MessageBoxW(g_dialog, L"已移除导入的方案。若有方案正在使用，请重新选择并应用一个内置方案。",
                L"星猫键道设置", MB_OK | MB_ICONINFORMATION);
}

void chooseStyle() {
    if (g_busy || g_syncingStyleCombo) return;
    const int index = static_cast<int>(SendDlgItemMessageW(g_dialog, IDC_STYLE_LIST, CB_GETCURSEL, 0, 0));
    applyStyleByIndex(index);
}

void importStyle() {
    if (g_busy) return;
    const auto file = pickFile(L"导入星猫候选皮肤（.json）", L"*.json");
    if (!file) return;
    std::wstring error;
    if (!importStyleFile(*file, error)) {
        showError(error);
        return;
    }
    refreshStyleList();
    const int index = static_cast<int>(g_styleList.size()) - 1;
    if (index >= 0) {
        SendDlgItemMessageW(g_dialog, IDC_STYLE_LIST, CB_SETCURSEL, static_cast<WPARAM>(index), 0);
        applyStyleByIndex(index);
    }
    MessageBoxW(g_dialog, L"皮肤已导入并启用。切换一次输入即可看到新样式。", L"星猫键道设置",
                MB_OK | MB_ICONINFORMATION);
}

void exportStyle() {
    if (g_busy) return;
    const auto target = askSavePath(L"导出当前候选皮肤", xmjd::win::widen(g_styles.active.id) + L".json");
    if (!target) return;
    std::string error;
    if (!xmjd::writeCandidateStyleFile(g_styles.active, *target, error)) {
        showError(xmjd::win::widen(error));
        return;
    }
    MessageBoxW(g_dialog, L"皮肤已导出。可以复制到 Mac 版星猫的候选样式目录，或分享给别人。",
                L"星猫键道设置", MB_OK | MB_ICONINFORMATION);
}

void openPath(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// 把当前对话框截成 PNG。自检用：布局是否错位、预览是否画得出来，看一眼就知道。
bool pngEncoderClsid(CLSID& clsid);

void captureSelfTest(HWND dialog) {
    RECT bounds{};
    if (!GetWindowRect(dialog, &bounds)) return;
    const int width = bounds.right - bounds.left;
    const int height = bounds.bottom - bounds.top;
    if (width <= 0 || height <= 0) return;
    HDC screen = GetDC(nullptr);
    if (!screen) return;
    HDC memory = CreateCompatibleDC(screen);
    if (!memory) {
        ReleaseDC(nullptr, screen);
        return;
    }
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(memory, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bitmap || !bits) {
        DeleteDC(memory);
        ReleaseDC(nullptr, screen);
        return;
    }
    HGDIOBJ previous = SelectObject(memory, bitmap);
    PrintWindow(dialog, memory, PW_RENDERFULLCONTENT);
    // 自绘的预览框不会被 PrintWindow 画出来（静态控件是空的），按控件位置补画。
    if (HWND preview = GetDlgItem(dialog, IDC_STYLE_PREVIEW)) {
        RECT place{};
        if (GetWindowRect(preview, &place)) {
            RECT local{place.left - bounds.left, place.top - bounds.top,
                       place.left - bounds.left + (place.right - place.left),
                       place.top - bounds.top + (place.bottom - place.top)};
            Gdiplus::Graphics graphics(memory);
            const int dpi = static_cast<int>(GetDpiForWindow(dialog));
            xmjd::preview::paint(graphics, local, g_styles.active, dpi == 0 ? 96 : dpi, g_previewDark);
        }
    }
    // GDI 不写 alpha，统一补成不透明再存 PNG。
    auto* pixels = static_cast<std::uint32_t*>(bits);
    for (int index = 0; index < width * height; ++index) pixels[index] |= 0xFF000000u;
    Gdiplus::GdiplusStartupInput input;
    ULONG_PTR token = 0;
    if (Gdiplus::GdiplusStartup(&token, &input, nullptr) == Gdiplus::Ok) {
        CLSID png{};
        if (pngEncoderClsid(png)) {
            Gdiplus::Bitmap surface(width, height, width * 4, PixelFormat32bppPARGB, static_cast<BYTE*>(bits));
            const auto path = g_dataRoot / L"Logs" / L"ui-selftest.png";
            std::error_code ec;
            std::filesystem::create_directories(path.parent_path(), ec);
            surface.Save(path.c_str(), &png, nullptr);
        }
        Gdiplus::GdiplusShutdown(token);
    }
    SelectObject(memory, previous);
    DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);
}

// MARK: - 外观
//
// 按 impeccable 的 craft floor：不用卡片当页面容器（"cards are the lazy container"），
// 不用彩色 border-left 装饰，主色只在候选预览里（那是内容不是装饰）。
// 分组全靠间距与字重：栏位之间 12 DLU，标题距离自己的控件 14 DLU、距上一块 18+ DLU。

struct SectionTitle {
    int x;
    int y;
    const wchar_t* text;
};

constexpr SectionTitle kSectionTitles[] = {
    {10, 126, L"输入方案"},
    {10, 220, L"方案来源"},
    {162, 126, L"候选皮肤"},
    {312, 126, L"配色"},
};

// 暖调中性色：不用纯黑纯灰，全部带一点暖黄，和橙色主色同一族。
constexpr COLORREF kColourAppBackground = RGB(0xF6, 0xF4, 0xF1);
constexpr COLORREF kColourWellFill = RGB(0xE9, 0xE6, 0xE1);
constexpr COLORREF kColourWellBorder = RGB(0xDE, 0xD9, 0xD2);
constexpr COLORREF kColourPrimaryText = RGB(0x1F, 0x1D, 0x1B);
constexpr COLORREF kColourSecondaryText = RGB(0x6B, 0x66, 0x60);

RECT g_previewWell{};      // 预览"画框"（像素），由 .rc 的 CONTROL 位置推出
HFONT g_titleFont = nullptr;
HFONT g_sectionFont = nullptr;
HBRUSH g_backgroundBrush = nullptr;

void createChromeResources(HWND dialog) {
    if (!g_backgroundBrush) g_backgroundBrush = CreateSolidBrush(kColourAppBackground);
    const int dpi = static_cast<int>(GetDpiForWindow(dialog));
    const int scale = dpi == 0 ? 96 : dpi;
    if (!g_titleFont) {
        LOGFONTW format{};
        // 字阶：标题 20px / 小节 11px / 正文 12px。
        format.lfHeight = -MulDiv(20, scale, 96);
        format.lfWeight = FW_SEMIBOLD;
        format.lfCharSet = DEFAULT_CHARSET;
        wcscpy_s(format.lfFaceName, L"Microsoft YaHei UI");
        g_titleFont = CreateFontIndirectW(&format);
    }
    if (!g_sectionFont) {
        LOGFONTW format{};
        format.lfHeight = -MulDiv(11, scale, 96);
        format.lfWeight = FW_SEMIBOLD;
        format.lfCharSet = DEFAULT_CHARSET;
        wcscpy_s(format.lfFaceName, L"Microsoft YaHei UI");
        g_sectionFont = CreateFontIndirectW(&format);
    }
}

// 预览控件外面留 6 DLU 的画框，让"这是候选窗的取景"一眼可读。
void layoutPreviewWell(HWND dialog) {
    RECT control{16, 60, 16 + 408, 60 + 46};
    MapDialogRect(dialog, &control);
    g_previewWell = control;
    InflateRect(&g_previewWell, 6, 6);
}

void fillRoundedCard(Gdiplus::Graphics& graphics, const RECT& rect, int radius,
                     const Gdiplus::Color& color) {
    Gdiplus::GraphicsPath path;
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    const int limit = std::min({radius, width / 2, height / 2});
    if (limit <= 0) {
        path.AddRectangle(Gdiplus::Rect(rect.left, rect.top, width, height));
    } else {
        const int d = limit * 2;
        path.AddArc(rect.left, rect.top, d, d, 180, 90);
        path.AddArc(rect.right - d, rect.top, d, d, 270, 90);
        path.AddArc(rect.right - d, rect.bottom - d, d, d, 0, 90);
        path.AddArc(rect.left, rect.bottom - d, d, d, 90, 90);
        path.CloseFigure();
    }
    Gdiplus::SolidBrush brush(color);
    graphics.FillPath(&brush, &path);
}

void strokeRoundedCard(Gdiplus::Graphics& graphics, const RECT& rect, int radius,
                       const Gdiplus::Color& color) {
    Gdiplus::GraphicsPath path;
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    const int limit = std::min({radius, width / 2, height / 2});
    if (limit <= 0) return;
    const int d = limit * 2;
    path.AddArc(rect.left, rect.top, d, d, 180, 90);
    path.AddArc(rect.right - d, rect.top, d, d, 270, 90);
    path.AddArc(rect.right - d, rect.bottom - d, d, d, 0, 90);
    path.AddArc(rect.left, rect.bottom - d, d, d, 90, 90);
    path.CloseFigure();
    Gdiplus::Pen pen(color, 1.0f);
    graphics.DrawPath(&pen, &path);
}

void paintChrome(HWND dialog) {
    PAINTSTRUCT paint{};
    HDC dc = BeginPaint(dialog, &paint);
    RECT client{};
    GetClientRect(dialog, &client);
    FillRect(dc, &client, g_backgroundBrush ? g_backgroundBrush : reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1));

    {
        Gdiplus::Graphics graphics(dc);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        // 预览画框：内凹的浅色井，带 1px 描边，没有投影——它不是浮起的卡片。
        fillRoundedCard(graphics, g_previewWell, 8, Gdiplus::Color(255, 0xE9, 0xE6, 0xE1));
        strokeRoundedCard(graphics, g_previewWell, 8, Gdiplus::Color(255, 0xDE, 0xD9, 0xD2));
    }

    const HGDIOBJ previous = SelectObject(dc, g_sectionFont ? g_sectionFont : GetStockObject(DEFAULT_GUI_FONT));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, kColourSecondaryText);
    for (const SectionTitle& section : kSectionTitles) {
        RECT label{0, 0, 0, 0};
        label.left = section.x;
        label.top = section.y;
        label.right = section.x + 140;
        label.bottom = section.y + 12;
        MapDialogRect(dialog, &label);
        DrawTextW(dc, section.text, -1, &label, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    }
    SelectObject(dc, previous);
    EndPaint(dialog, &paint);
}


// MARK: - 颜色编辑

void paintSwatch(const DRAWITEMSTRUCT& item) {
    RECT bounds = item.rcItem;
    FillRect(item.hDC, &bounds,
             g_backgroundBrush ? g_backgroundBrush : reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1));
    const xmjd::StyleColor* slot = colourSlot(g_styles.active, static_cast<int>(item.CtlID), g_previewDark);
    if (!slot) return;
    const COLORREF rgb = styleColour(*slot);
    Gdiplus::Graphics graphics(item.hDC);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    RECT inner = bounds;
    InflateRect(&inner, -1, -1);
    fillRoundedCard(graphics, inner, 4,
                    Gdiplus::Color(255, GetRValue(rgb), GetGValue(rgb), GetBValue(rgb)));
    // 描边深一点：白色/近白色块在白卡上全靠这一圈才看得出来。
    Gdiplus::Pen border(Gdiplus::Color(255, 199, 199, 204), 1.0f);
    graphics.DrawRectangle(&border, static_cast<INT>(inner.left), static_cast<INT>(inner.top),
                           static_cast<INT>(inner.right - inner.left - 1),
                           static_cast<INT>(inner.bottom - inner.top - 1));
}

void pickColour(int id) {
    xmjd::StyleColor* slot = colourSlot(g_styles.active, id, g_previewDark);
    if (!slot) return;
    static COLORREF custom[16]{};
    CHOOSECOLORW choose{};
    choose.lStructSize = sizeof(choose);
    choose.hwndOwner = g_dialog;
    choose.rgbResult = styleColour(*slot);
    choose.lpCustColors = custom;
    choose.Flags = CC_FULLOPEN | CC_RGBINIT | CC_ANYCOLOR;
    if (!ChooseColorW(&choose)) return;
    const double alpha = slot->alpha;
    slot->red = GetRValue(choose.rgbResult) / 255.0;
    slot->green = GetGValue(choose.rgbResult) / 255.0;
    slot->blue = GetBValue(choose.rgbResult) / 255.0;
    slot->alpha = alpha == 0.0 ? 1.0 : alpha;   // 颜色选好了就别再全透明
    if (saveActiveStyleEdit()) {
        repaintStylePreview();
        InvalidateRect(GetDlgItem(g_dialog, id), nullptr, TRUE);
    }
}

// MARK: - 单实例

constexpr wchar_t kActivateMessageName[] = L"XMJD.Settings.Activate";
constexpr wchar_t kSettingsDialogClass[] = L"#32770";
constexpr wchar_t kSettingsDialogTitle[] = L"星猫键道设置";

INT_PTR CALLBACK correctionProc(HWND dialog, UINT message, WPARAM wParam, LPARAM) {
    const auto enableControls = [dialog] {
        const BOOL on = IsDlgButtonChecked(dialog, IDC_CORRECTION_ENABLED) == BST_CHECKED;
        EnableWindow(GetDlgItem(dialog, IDC_CORRECTION_STRENGTH), on);
        EnableWindow(GetDlgItem(dialog, IDC_CORRECTION_LEAD), on);
        EnableWindow(GetDlgItem(dialog, IDC_CORRECTION_SINGLE_CHARACTER), on);
    };
    if (message == WM_INITDIALOG) {
        const auto prefs = xmjd::loadCorrectionPreferences(g_dataRoot);
        CheckDlgButton(dialog, IDC_CORRECTION_ENABLED, prefs.enabled ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(dialog, IDC_CORRECTION_SINGLE_CHARACTER, prefs.singleCharacter ? BST_CHECKED : BST_UNCHECKED);
        SetDlgItemInt(dialog, IDC_CORRECTION_STRENGTH, prefs.strength, FALSE);
        SendDlgItemMessageW(dialog, IDC_CORRECTION_STRENGTH, EM_SETLIMITTEXT, 3, 0);
        for (int v = 10; v <= 30; ++v) {
            const auto label = std::to_wstring(v / 10) + L"." + std::to_wstring(v % 10) + L" 倍";
            SendDlgItemMessageW(dialog, IDC_CORRECTION_LEAD, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
        }
        SendDlgItemMessageW(dialog, IDC_CORRECTION_LEAD, CB_SETCURSEL, prefs.leadTenths - 10, 0);
        enableControls();
        return TRUE;
    }
    if (message != WM_COMMAND) return FALSE;
    switch (LOWORD(wParam)) {
    case IDC_CORRECTION_ENABLED:
        enableControls();
        return TRUE;
    case IDC_CORRECTION_RESET:
        if (MessageBoxW(dialog, L"只清除上下文纠错的选词和忽略记录，不影响词库。清除后无法恢复。", L"清除纠错学习记录？",
                        MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES) {
            auto prefs = xmjd::loadCorrectionPreferences(g_dataRoot);
            FILETIME now{};
            GetSystemTimeAsFileTime(&now);
            prefs.learningReset = (static_cast<std::uint64_t>(now.dwHighDateTime) << 32) | now.dwLowDateTime;
            if (!xmjd::saveCorrectionPreferences(g_dataRoot, prefs))
                MessageBoxW(dialog, L"无法保存，请检查用户目录。", L"上下文纠错", MB_OK | MB_ICONERROR);
            else MessageBoxW(dialog, L"将在下次用键道6输入时清除纠错学习记录。", L"上下文纠错", MB_OK | MB_ICONINFORMATION);
        }
        return TRUE;
    case IDOK: {
        BOOL valid = FALSE;
        const UINT strength = GetDlgItemInt(dialog, IDC_CORRECTION_STRENGTH, &valid, FALSE);
        const auto lead = SendDlgItemMessageW(dialog, IDC_CORRECTION_LEAD, CB_GETCURSEL, 0, 0);
        if (!valid || strength > 100 || lead < 0 || lead > 20) {
            MessageBoxW(dialog, L"请输入 0–100 的纠错强度并选择领先倍数。", L"上下文纠错", MB_OK | MB_ICONWARNING);
            return TRUE;
        }
        auto prefs = xmjd::loadCorrectionPreferences(g_dataRoot);
        prefs.enabled = IsDlgButtonChecked(dialog, IDC_CORRECTION_ENABLED) == BST_CHECKED;
        prefs.singleCharacter = IsDlgButtonChecked(dialog, IDC_CORRECTION_SINGLE_CHARACTER) == BST_CHECKED;
        prefs.strength = static_cast<int>(strength);
        prefs.leadTenths = static_cast<int>(lead) + 10;
        if (!xmjd::saveCorrectionPreferences(g_dataRoot, prefs)) {
            MessageBoxW(dialog, L"无法保存，请检查用户目录。", L"上下文纠错", MB_OK | MB_ICONERROR);
            return TRUE;
        }
        EndDialog(dialog, IDOK);
        return TRUE;
    }
    case IDCANCEL:
        EndDialog(dialog, IDCANCEL);
        return TRUE;
    default:
        return FALSE;
    }
}

INT_PTR CALLBACK settingsProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam) {
    (void)lParam;
    // 另一个实例点了"设置"：把自己提到前面，不再另开窗口。
    static const UINT activateMessage = RegisterWindowMessageW(kActivateMessageName);
    if (activateMessage != 0 && message == activateMessage) {
        if (IsIconic(dialog)) ShowWindow(dialog, SW_RESTORE);
        SetForegroundWindow(dialog);
        FlashWindow(dialog, TRUE);
        return TRUE;
    }
    switch (message) {
    case WM_INITDIALOG: {
        g_dialog = dialog;
        createChromeResources(dialog);
        layoutPreviewWell(dialog);
        if (g_titleFont) SendDlgItemMessageW(dialog, IDC_TITLE, WM_SETFONT,
                                            reinterpret_cast<WPARAM>(g_titleFont), TRUE);
        const HICON icon = LoadIconW(g_instance, MAKEINTRESOURCEW(1));
        if (icon) {
            SendMessageW(dialog, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(icon));
            SendMessageW(dialog, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(icon));
            SendDlgItemMessageW(dialog, IDC_APPICON, STM_SETICON, reinterpret_cast<WPARAM>(icon), 0);
        }
        SetDlgItemTextW(dialog, IDC_VERSION, (L"版本 " + xmjd::win::widen(XMJD_VERSION_FULL)).c_str());
        {
            RECT dialogRect{};
            MONITORINFO monitor{sizeof(monitor)};
            if (GetWindowRect(dialog, &dialogRect) &&
                GetMonitorInfoW(MonitorFromWindow(dialog, MONITOR_DEFAULTTONEAREST), &monitor)) {
                const int width = dialogRect.right - dialogRect.left;
                const int height = dialogRect.bottom - dialogRect.top;
                const int x = monitor.rcWork.left + (monitor.rcWork.right - monitor.rcWork.left - width) / 2;
                const int y = monitor.rcWork.top + (monitor.rcWork.bottom - monitor.rcWork.top - height) / 2;
                SetWindowPos(dialog, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            }
        }
        refreshSchemaList(-1);
        refreshSourceLabel();
        refreshStyleList();
        refreshStatus();
        if (isElevated()) {
            setStatus(L"当前是管理员窗口；引擎和用户配置必须属于普通桌面用户。请关闭后从开始菜单以普通身份打开。");
            setBusy(false);
        }
        if (g_selfTest) {
            for (const int id : {IDC_SCHEMA_LIST, IDC_APPLY, IDC_REDEPLOY, IDC_START_HOST, IDC_OPEN_USER,
                                 IDC_OPEN_LOG, IDC_OPEN_LANGUAGE, IDC_STATUS, IDC_SOURCE_LABEL, IDC_CHOOSE_SOURCE,
                                 IDC_OPEN_SOURCE, IDC_REMOVE_SOURCE, IDC_STYLE_LIST, IDC_IMPORT_STYLE,
                                 IDC_EXPORT_STYLE, IDC_OPEN_STYLES, IDC_STYLE_DETAIL, IDC_STYLE_PREVIEW,
                                 IDC_STYLE_FONT, IDC_STYLE_LAYOUT, IDC_STYLE_SIZE, IDC_STYLE_ROW,
                                 IDC_STYLE_RADIUS, IDC_STYLE_OPACITY, IDC_STYLE_MAXWIDTH, IDC_STYLE_LABELS,
                                 IDC_STYLE_COMMENTS, IDC_STYLE_DARK, IDC_STYLE_RESET, IDC_STYLE_COMMENT_SIZE,
                                 IDC_STYLE_PADDING, IDC_STYLE_ITEM_SPACING, IDC_STYLE_PAD_H, IDC_STYLE_PAD_V,
                                 IDC_STYLE_ITEM_RADIUS, IDC_COLOR_BG, IDC_COLOR_TEXT, IDC_COLOR_SELECTED_TEXT,
                                 IDC_COLOR_SECONDARY, IDC_COLOR_HIGHLIGHT_START, IDC_COLOR_HIGHLIGHT_END,
                                 IDC_COLOR_BG_END, IDC_CORRECTION_SETTINGS}) {
                if (!GetDlgItem(dialog, id)) {
                    EndDialog(dialog, 0);
                    return TRUE;
                }
            }
            const HWND correctionDialog = CreateDialogParamW(
                reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(dialog, GWLP_HINSTANCE)),
                MAKEINTRESOURCEW(IDD_CORRECTION), dialog, correctionProc, 0);
            const bool correctionControls = correctionDialog &&
                GetDlgItem(correctionDialog, IDC_CORRECTION_ENABLED) &&
                GetDlgItem(correctionDialog, IDC_CORRECTION_STRENGTH) &&
                GetDlgItem(correctionDialog, IDC_CORRECTION_RESET) &&
                GetDlgItem(correctionDialog, IDC_CORRECTION_SINGLE_CHARACTER) &&
                SendDlgItemMessageW(correctionDialog, IDC_CORRECTION_LEAD, CB_GETCOUNT, 0, 0) == 21;
            if (correctionDialog) DestroyWindow(correctionDialog);
            if (!correctionControls) {
                EndDialog(dialog, 0);
                return TRUE;
            }
            // 记下真实像素尺寸：对话框按 DLU 布局，缩放到具体 DPI 后是否还装得下
            // 只能实测，不然预览框被压窄了也看不出来。
            RECT client{};
            GetClientRect(dialog, &client);
            std::wstring metrics = L"ui self-test: dialog client " + std::to_wstring(client.right) + L"x" +
                                   std::to_wstring(client.bottom);
            for (const int id : {IDC_STYLE_PREVIEW, IDC_STYLE_LIST, IDC_SCHEMA_LIST}) {
                RECT item{};
                if (HWND control = GetDlgItem(dialog, id)) {
                    GetClientRect(control, &item);
                    metrics += L"; ctl " + std::to_wstring(id) + L" " + std::to_wstring(item.right) + L"x" +
                               std::to_wstring(item.bottom);
                }
            }
            appendLog(metrics);
            SetTimer(dialog, 1, 300, nullptr);
        }
        return TRUE;
    }
    case WM_ERASEBKGND:
        return 1;   // 背景统一在 WM_PAINT 里画，避免先刷一遍灰再盖住。
    case WM_PAINT:
        paintChrome(dialog);
        return TRUE;
    case WM_CTLCOLORDLG:
        return reinterpret_cast<INT_PTR>(g_backgroundBrush ? g_backgroundBrush
                                                          : reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1));
    case WM_CTLCOLORSTATIC: {
        HDC dc = reinterpret_cast<HDC>(wParam);
        HWND control = reinterpret_cast<HWND>(lParam);
        SetBkMode(dc, TRANSPARENT);
        const int id = control ? GetDlgCtrlID(control) : 0;
        const bool muted = id == IDC_VERSION || id == IDC_STATUS || id == IDC_SCHEMA_HINT ||
                           id == IDC_SCHEMA_HELP || id == IDC_COLOR_HINT || id == IDC_STYLE_DETAIL;
        SetTextColor(dc, muted ? kColourSecondaryText : kColourPrimaryText);
        return reinterpret_cast<INT_PTR>(g_backgroundBrush ? g_backgroundBrush
                                                          : reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1));
    }
    case WM_DRAWITEM: {
        const auto* item = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
        if (item && item->CtlID == IDC_STYLE_PREVIEW && g_dialog) {
            RECT bounds = item->rcItem;
            FillRect(item->hDC, &bounds,
                     g_backgroundBrush ? g_backgroundBrush : reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1));
            const int dpi = static_cast<int>(GetDpiForWindow(g_dialog));
            xmjd::preview::paint(item->hDC, bounds, g_styles.active, dpi == 0 ? 96 : dpi, g_previewDark);
            return TRUE;
        }
        if (item && isColourControl(static_cast<int>(item->CtlID))) {
            paintSwatch(*item);
            return TRUE;
        }
        return FALSE;
    }
    case WM_TIMER:
        if (g_selfTest && wParam == 1) {
            KillTimer(dialog, 1);
            captureSelfTest(dialog);
            EndDialog(dialog, 1);
            return TRUE;
        }
        return FALSE;
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDC_CORRECTION_SETTINGS:
            DialogBoxParamW(reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(dialog, GWLP_HINSTANCE)),
                            MAKEINTRESOURCEW(IDD_CORRECTION), dialog, correctionProc, 0);
            return TRUE;
        case IDC_APPLY: applySelection(); return TRUE;
        case IDC_REDEPLOY: redeployOnly(); return TRUE;
        case IDC_START_HOST: startEngine(); return TRUE;
        case IDC_CHOOSE_SOURCE: chooseSchemeSource(); return TRUE;
        case IDC_OPEN_SOURCE: openSchemeSource(); return TRUE;
        case IDC_REMOVE_SOURCE: removeSchemeSource(); return TRUE;
        case IDC_IMPORT_STYLE: importStyle(); return TRUE;
        case IDC_EXPORT_STYLE: exportStyle(); return TRUE;
        case IDC_OPEN_STYLES: openPath(xmjd::candidateStyleDirectory(g_dataRoot)); return TRUE;
        case IDC_STYLE_LIST:
            if (HIWORD(wParam) == CBN_SELCHANGE) chooseStyle();
            return TRUE;
        case IDC_STYLE_FONT:
        case IDC_STYLE_LAYOUT:
            if (HIWORD(wParam) == CBN_SELCHANGE) applyStyleEdits();
            return TRUE;
        case IDC_STYLE_SIZE:
        case IDC_STYLE_ROW:
        case IDC_STYLE_RADIUS:
        case IDC_STYLE_OPACITY:
        case IDC_STYLE_MAXWIDTH:
        case IDC_STYLE_COMMENT_SIZE:
        case IDC_STYLE_PADDING:
        case IDC_STYLE_ITEM_SPACING:
        case IDC_STYLE_PAD_H:
        case IDC_STYLE_PAD_V:
        case IDC_STYLE_ITEM_RADIUS:
            if (HIWORD(wParam) == EN_CHANGE) applyStyleEdits();
            return TRUE;
        case IDC_COLOR_BG:
        case IDC_COLOR_TEXT:
        case IDC_COLOR_SELECTED_TEXT:
        case IDC_COLOR_SECONDARY:
        case IDC_COLOR_HIGHLIGHT_START:
        case IDC_COLOR_HIGHLIGHT_END:
        case IDC_COLOR_BG_END:
            pickColour(LOWORD(wParam));
            return TRUE;
        case IDC_STYLE_LABELS:
        case IDC_STYLE_COMMENTS:
            applyStyleEdits();
            return TRUE;
        case IDC_STYLE_DARK:
            g_previewDark = IsDlgButtonChecked(g_dialog, IDC_STYLE_DARK) == BST_CHECKED;
            repaintStylePreview();
            return TRUE;
        case IDC_STYLE_RESET:
            resetActiveStyle();
            return TRUE;
        case IDC_OPEN_USER: openPath(g_dataRoot); return TRUE;
        case IDC_OPEN_LOG: openPath(g_dataRoot / L"Logs"); return TRUE;
        case IDC_OPEN_LANGUAGE:
            ShellExecuteW(nullptr, L"open", L"ms-settings:regionlanguage", nullptr, nullptr, SW_SHOWNORMAL);
            return TRUE;
        case IDCANCEL: EndDialog(dialog, 0); return TRUE;
        default: break;
        }
        return FALSE;
    case WM_CLOSE:
        EndDialog(dialog, 0);
        return TRUE;
    default:
        return FALSE;
    }
}

int runCheck() {
    if (!loadSchemas()) return 2;
    const std::string selected = readSelectedSchema();
    if (settingsValue("scheme_source").empty() && !selected.empty() &&
        std::none_of(g_schemas.begin(), g_schemas.end(), [&](const auto& schema) { return schema.id == selected; })) {
        return 3;
    }
    const auto styles = xmjd::loadCandidateStyleCatalog(g_dataRoot);
    if (!xmjd::validCandidateStyle(styles.active)) return 4;
    return 0;
}

int runSelect(const std::string& schema) {
    if (!loadSchemas()) return 2;
    std::wstring error;
    if (!writeConfiguration(schema, error)) return 3;
    return 0;
}

int runSelectStyle(const std::string& id) {
    const auto styles = xmjd::loadCandidateStyleCatalog(g_dataRoot);
    const auto found = std::find_if(styles.builtin.begin(), styles.builtin.end(),
                                    [&](const auto& style) { return style.id == id; });
    const auto custom = std::find_if(styles.custom.begin(), styles.custom.end(),
                                     [&](const auto& style) { return style.id == id; });
    if (found == styles.builtin.end() && custom == styles.custom.end()) return 3;
    const xmjd::CandidateStyle& style = found != styles.builtin.end() ? *found : *custom;
    std::string error;
    if (!xmjd::activateCandidateStyle(g_dataRoot, style, error)) return 4;
    updateSettings({{"style", style.id}});
    return 0;
}

int runImportScheme(const std::filesystem::path& source) {
    std::wstring error;
    std::size_t schemas = 0;
    if (!setSchemeSource(source, error, schemas)) {
        std::fwprintf(stderr, L"import failed: %ls\n", error.c_str());
        return 3;
    }
    if (!loadSchemas()) return 2;
    std::fwprintf(stdout, L"imported %zu schemas\n", schemas);
    return 0;
}
bool pngEncoderClsid(CLSID& clsid) {
    UINT count = 0;
    UINT size = 0;
    if (Gdiplus::GetImageEncodersSize(&count, &size) != Gdiplus::Ok || size == 0) return false;
    std::vector<BYTE> storage(size);
    auto* codecs = reinterpret_cast<Gdiplus::ImageCodecInfo*>(storage.data());
    if (Gdiplus::GetImageEncoders(count, size, codecs) != Gdiplus::Ok) return false;
    for (UINT index = 0; index < count; ++index) {
        if (codecs[index].MimeType && wcscmp(codecs[index].MimeType, L"image/png") == 0) {
            clsid = codecs[index].Clsid;
            return true;
        }
    }
    return false;
}

// 把内置皮肤逐套渲染成 PNG。设置页的预览用的就是同一份绘制代码，所以这组图
// 等同于候选窗的实际效果，可以在不看屏幕的情况下核对视觉。
int runRenderStyles(const std::filesystem::path& directory) {
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    Gdiplus::GdiplusStartupInput input;
    ULONG_PTR token = 0;
    if (Gdiplus::GdiplusStartup(&token, &input, nullptr) != Gdiplus::Ok) return 5;
    CLSID png{};
    if (!pngEncoderClsid(png)) {
        Gdiplus::GdiplusShutdown(token);
        return 6;
    }
    constexpr int kWidth = 720;
    constexpr int kHeight = 200;
    int failures = 0;
    for (const auto& style : xmjd::builtinCandidateStyles()) {
        for (const bool dark : {false, true}) {
            Gdiplus::Bitmap bitmap(kWidth, kHeight, PixelFormat32bppARGB);
            {
                Gdiplus::Graphics graphics(&bitmap);
                graphics.Clear(dark ? Gdiplus::Color(255, 32, 32, 36) : Gdiplus::Color(255, 236, 236, 239));
                const RECT bounds{0, 0, kWidth, kHeight};
                xmjd::preview::paint(graphics, bounds, style, 96, dark);
            }
            const std::wstring name = xmjd::win::widen(style.id) + (dark ? L"-dark.png" : L"-light.png");
            if (bitmap.Save((directory / name).c_str(), &png, nullptr) != Gdiplus::Ok) ++failures;
        }
    }
    Gdiplus::GdiplusShutdown(token);
    return failures == 0 ? 0 : 7;
}
} // namespace

// MARK: - 单实例

// 把已经在跑的那个设置页叫到前面：先广播专用消息让它自己置前，
// 广播落空（对方窗口还没建出来）再按标题兜底找一次。
void raiseRunningSettingsWindow() {
    AllowSetForegroundWindow(ASFW_ANY);
    const UINT activate = RegisterWindowMessageW(kActivateMessageName);
    if (activate != 0) {
        DWORD_PTR acknowledged = 0;
        SendMessageTimeoutW(HWND_BROADCAST, activate, 0, 0, SMTO_ABORTIFHUNG, 400, &acknowledged);
    }
    if (HWND existing = FindWindowW(kSettingsDialogClass, kSettingsDialogTitle)) {
        if (IsIconic(existing)) ShowWindow(existing, SW_RESTORE);
        SetForegroundWindow(existing);
    }
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    g_instance = instance;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const auto module = xmjd::win::modulePath();
    if (module.empty()) return 1;
    g_installDirectory = module.parent_path();
    g_dataRoot = xmjd::win::dataRoot();
    if (g_dataRoot.empty()) return 1;

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return 1;
    int result = -1;
    if (argc >= 2 && _wcsicmp(argv[1], L"--check") == 0) {
        result = runCheck();
    } else if (argc >= 3 && _wcsicmp(argv[1], L"--select") == 0) {
        result = runSelect(xmjd::win::narrow(argv[2]));
    } else if (argc >= 3 && _wcsicmp(argv[1], L"--select-style") == 0) {
        result = runSelectStyle(xmjd::win::narrow(argv[2]));
    } else if (argc >= 3 && _wcsicmp(argv[1], L"--import-scheme") == 0) {
        result = runImportScheme(std::filesystem::path(argv[2]));
    } else if (argc >= 2 && _wcsicmp(argv[1], L"--self-test-ui") == 0) {
        g_selfTest = true;
    } else if (argc >= 3 && _wcsicmp(argv[1], L"--render-styles") == 0) {
        result = runRenderStyles(std::filesystem::path(argv[2]));
    } else if (argc >= 2 && _wcsicmp(argv[1], L"--styles") == 0) {
        const auto styles = xmjd::loadCandidateStyleCatalog(g_dataRoot);
        printLine(L"active=" + xmjd::win::widen(styles.active.id));
        for (const auto& style : styles.builtin) {
            printLine(L"builtin " + xmjd::win::widen(style.id) + L" " + xmjd::win::widen(style.name));
        }
        for (const auto& style : styles.custom) {
            printLine(L"custom " + xmjd::win::widen(style.id) + L" " + xmjd::win::widen(style.name));
        }
        result = 0;
    } else if (argc >= 2 && _wcsicmp(argv[1], L"--schemas") == 0) {
        if (!loadSchemas()) {
            result = 2;
        } else {
            for (const auto& schema : g_schemas) {
                printLine(L"schema " + xmjd::win::widen(schema.id) + L" " + schema.name +
                          (schema.fromUserDirectory ? L" imported" : L""));
            }
            result = 0;
        }
    } else if (argc >= 2 && _wcsicmp(argv[1], L"--remove-schemes") == 0) {
        std::wstring error;
        result = removeImportedSchemes(error) ? 0 : 3;
    }
    LocalFree(argv);
    if (result >= 0) {
        if (com == S_OK || com == S_FALSE) CoUninitialize();
        return result;
    }
    // File pickers below need the apartment initialized by wWinMain.

    // 交互界面只允许一个实例：托盘/开始菜单连点几下，应该把已有窗口叫到前面，
    // 而不是叠出一堆一模一样的设置页。诊断用的 --self-test-ui 不受此限。
    HANDLE instanceMutex = nullptr;
    if (!g_selfTest) {
        instanceMutex = CreateMutexW(nullptr, TRUE, L"Local\\XMJD.Settings.SingleInstance");
        if (!instanceMutex) return 1;
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            raiseRunningSettingsWindow();
            CloseHandle(instanceMutex);
            if (com == S_OK || com == S_FALSE) CoUninitialize();
            return 0;
        }
    }
    (void)instanceMutex;   // 持有到进程退出，不显式释放。

    if (!loadSchemas()) {
        MessageBoxW(nullptr, L"无法读取输入方案。请确认程序目录中的 Resources\\Rime 完整。",
                    L"星猫键道设置", MB_OK | MB_ICONERROR);
        return 1;
    }
    INITCOMMONCONTROLSEX controls{static_cast<DWORD>(sizeof(controls)), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);
    const INT_PTR dialogResult = DialogBoxParamW(instance, MAKEINTRESOURCEW(IDD_SETTINGS), nullptr, settingsProc, 0);
    if (g_selfTest) return dialogResult == 1 ? 0 : 1;
    return dialogResult == -1 ? 1 : 0;
}
