#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <cctype>
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
#include "../Common/CandidateStyle.h"
#include "../Common/Pipe.h"
#include "../Protocol/Protocol.h"

using Microsoft::WRL::ComPtr;

namespace {
constexpr std::size_t kMaxSchemaBytes = 128 * 1024;
constexpr DWORD kDeployTimeoutMs = 10 * 60 * 1000;
constexpr std::size_t kMaxImportFiles = 20000;
constexpr std::uintmax_t kMaxImportBytes = 512u * 1024u * 1024u;

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
    scanDirectory(g_installDirectory / L"Resources" / L"Rime", false, false);
    scanDirectory(g_dataRoot / L"UserData" / L"Rime", true, true);
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
    return std::any_of(g_schemas.begin(), g_schemas.end(), [](const auto& schema) { return schema.id == "xmjd6"; });
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

bool writeConfiguration(const std::string& selected, std::wstring& error) {
    if (!validSchemaId(selected) ||
        std::none_of(g_schemas.begin(), g_schemas.end(), [&](const auto& schema) { return schema.id == selected; })) {
        error = L"未知的输入方案。";
        return false;
    }
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
    const auto response = client.transact(request, kDeployTimeoutMs, true);
    if (!response) {
        error = L"部署超时或引擎连接中断。请查看日志后重试。";
        appendLog(L"deploy transport failure");
        return false;
    }
    if (response->status == xmjd::Status::Ok && response->handled) {
        appendLog(L"deploy ok");
        return true;
    }
    const int code = static_cast<int>(response->snapshot.page);
    error = L"部署失败（" + xmjd::win::widen(xmjd::statusName(response->status)) +
            L"，错误码 " + std::to_wstring(code) + L"）。请查看日志后重试。";
    appendLog(L"deploy failed status=" + xmjd::win::widen(xmjd::statusName(response->status)) +
              L" code=" + std::to_wstring(code));
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

bool isRuntimeName(const std::wstring& name) {
    static const std::vector<std::wstring> excluded = {
        L"build", L"sync", L"trash", L"backups", L"staging", L"user.yaml", L"installation.yaml",
        L"zzc_state", L"runtime_undo.tsv", L"runtime_undo.tsv.bak", L"typing_stats.txt",
        L"typing_stats.txt.bak"};
    std::wstring lowered = name;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](wchar_t character) { return static_cast<wchar_t>(towlower(character)); });
    if (std::find(excluded.begin(), excluded.end(), lowered) != excluded.end()) return true;
    if (lowered.find(L".userdb") != std::wstring::npos) return true;
    if (lowered == L"zzc") return true;
    return false;
}

bool importableExtension(const std::filesystem::path& path) {
    std::wstring extension = path.extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](wchar_t character) { return static_cast<wchar_t>(towlower(character)); });
    static const std::vector<std::wstring> allowed = {
        L".yaml", L".yml", L".lua", L".json", L".ocd", L".ocd2", L".txt", L".tsv", L".gram",
        L".png", L".pdf", L".tif", L".tiff", L".ico"};
    return std::find(allowed.begin(), allowed.end(), extension) != allowed.end();
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

bool importSchemeDirectory(const std::filesystem::path& source, std::wstring& error, std::size_t& schemas) {
    schemas = 0;
    std::error_code ec;
    if (!std::filesystem::is_directory(source, ec) || ec) {
        error = L"请选择一个目录。";
        return false;
    }
    const auto resolved = std::filesystem::weakly_canonical(source, ec);
    if (ec) {
        error = L"无法读取所选目录。";
        return false;
    }
    const auto owned = std::filesystem::weakly_canonical(g_dataRoot, ec);
    if (ec) {
        error = L"无法解析用户数据目录。";
        return false;
    }
    if (isSubPath(owned, resolved) || isSubPath(resolved, owned)) {
        error = L"方案来源不能是星猫的运行目录，也不能是它的上级目录。请选择独立的 Rime 方案目录。";
        return false;
    }
    bool hasSchema = false;
    for (std::filesystem::directory_iterator it(resolved, ec), end; it != end && !ec; it.increment(ec)) {
        if (it->is_regular_file(ec) && endsWithSchemaYaml(it->path().filename().wstring())) {
            hasSchema = true;
            break;
        }
    }
    if (!hasSchema) {
        error = L"所选目录的根层没有 .schema.yaml。请选择方案文件所在的目录。";
        return false;
    }

    const auto destination = importedSchemesPath() / sanitizeFolderName(resolved.filename().wstring());
    if (std::filesystem::exists(destination, ec)) {
        std::filesystem::remove_all(destination, ec);
        if (ec) {
            error = L"无法清理上次导入的方案：" + destination.wstring();
            return false;
        }
    }
    std::filesystem::create_directories(destination, ec);
    if (ec) {
        error = L"无法创建导入目录：" + destination.wstring();
        return false;
    }
    std::size_t files = 0;
    std::uintmax_t bytes = 0;
    for (std::filesystem::recursive_directory_iterator it(
             resolved, std::filesystem::directory_options::skip_permission_denied, ec), end;
         it != end && !ec; it.increment(ec)) {
        std::error_code itemEc;
        if (it->is_symlink(itemEc)) continue;
        const auto name = it->path().filename().wstring();
        const bool directory = it->is_directory(itemEc);
        if (isRuntimeName(name)) {
            if (directory) it.disable_recursion_pending();
            continue;
        }
        if (directory) continue;
        if (!it->is_regular_file(itemEc) || itemEc) continue;
        if (!importableExtension(it->path())) continue;
        const auto size = it->file_size(itemEc);
        if (itemEc) continue;
        if (++files > kMaxImportFiles || bytes + size > kMaxImportBytes) {
            std::filesystem::remove_all(destination, ec);
            error = L"方案目录过大（上限 20,000 个文件 / 512 MiB）。请不要选择整个用户目录。";
            return false;
        }
        bytes += size;
        const auto relative = std::filesystem::relative(it->path(), resolved, ec);
        if (ec) continue;
        const auto target = destination / relative;
        std::filesystem::create_directories(target.parent_path(), ec);
        if (ec) continue;
        std::filesystem::copy_file(it->path(), target, std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            std::filesystem::remove_all(destination, ec);
            error = L"复制方案文件失败：" + relative.wstring();
            return false;
        }
        if (endsWithSchemaYaml(name)) ++schemas;
    }
    if (ec) {
        std::filesystem::remove_all(destination, ec);
        error = L"读取方案目录失败。请检查权限后重试。";
        return false;
    }
    if (schemas == 0) {
        std::filesystem::remove_all(destination, ec);
        error = L"没有找到可用的方案文件。";
        return false;
    }
    updateSettings({{"scheme_source", xmjd::win::narrow(resolved.wstring())}});
    appendLog(L"import scheme source=" + resolved.wstring() + L" schemas=" + std::to_wstring(schemas));
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
    loadSchemas();
    // A removed scheme cannot stay selected: fall back to the bundled one and
    // rewrite schema_list so the next deploy does not reference a missing file.
    const std::string selected = readSelectedSchema();
    if (!selected.empty() &&
        std::none_of(g_schemas.begin(), g_schemas.end(), [&](const auto& schema) { return schema.id == selected; })) {
        if (!writeConfiguration("xmjd6", error)) return false;
    }
    updateSettings({{"scheme_source", ""}});
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
        text = L"当前：内置方案（程序目录 Resources\\Rime）";
    } else {
        text = L"当前：" + abbreviatePath(xmjd::win::widen(source));
        std::error_code ec;
        if (!std::filesystem::is_directory(xmjd::win::widen(source), ec)) {
            text += L"（目录已不存在）";
        }
    }
    std::size_t imported = 0;
    for (const auto& schema : g_schemas) {
        if (schema.fromUserDirectory) ++imported;
    }
    if (imported > 0) text += L"；已导入 " + std::to_wstring(imported) + L" 个方案";
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

// MARK: - Candidate styles

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
    const std::wstring detail = L"当前：" + xmjd::win::widen(active.name) + L" · " +
                                (active.horizontal ? L"横排" : L"竖排") + L" · 字号 " +
                                std::to_wstring(static_cast<int>(active.fontSize)) + L" · 最大宽度 " +
                                std::to_wstring(static_cast<int>(active.maxWidth));
    SetDlgItemTextW(g_dialog, IDC_STYLE_DETAIL, detail.c_str());
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
    const std::wstring detail = L"已启用：" + xmjd::win::widen(g_styles.active.name) +
                                L"。下一句输入生效，无需重新部署。";
    SetDlgItemTextW(g_dialog, IDC_STYLE_DETAIL, detail.c_str());
    return true;
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

void chooseSchemeSource() {
    if (g_busy) return;
    const auto folder = pickFolder(L"选择 Rime 方案目录（根层需要 .schema.yaml）");
    if (!folder) return;
    std::wstring error;
    std::size_t schemas = 0;
    if (!importSchemeDirectory(*folder, error, schemas)) {
        showError(error);
        return;
    }
    if (!loadSchemas()) {
        showError(L"导入后没有读到内置方案 xmjd6，请检查程序目录是否完整。");
        return;
    }
    refreshSchemaList(-1);
    refreshSourceLabel();
    appendLog(L"scheme source ready: " + std::to_wstring(schemas));
    MessageBoxW(g_dialog,
                (L"已导入 " + std::to_wstring(schemas) + L" 个方案。\n\n"
                 L"在上面的列表里选中新方案，再点击“应用并部署”即可使用。").c_str(),
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
                    L"移除所有已导入的方案文件？\n\n内置方案不受影响；用户词库和设置保留。\n"
                    L"如果当前正在使用导入的方案，请先切换到内置方案再移除。",
                    L"星猫键道设置", MB_YESNO | MB_ICONQUESTION) != IDYES) {
        return;
    }
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

INT_PTR CALLBACK settingsProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam) {
    (void)lParam;
    switch (message) {
    case WM_INITDIALOG: {
        g_dialog = dialog;
        const HICON icon = LoadIconW(g_instance, MAKEINTRESOURCEW(1));
        if (icon) {
            SendMessageW(dialog, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(icon));
            SendMessageW(dialog, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(icon));
            SendDlgItemMessageW(dialog, IDC_APPICON, STM_SETICON, reinterpret_cast<WPARAM>(icon), 0);
        }
        SetDlgItemTextW(dialog, IDC_VERSION, (L"版本 " + xmjd::win::widen(XMJD_VERSION_FULL)).c_str());
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
                                 IDC_EXPORT_STYLE, IDC_OPEN_STYLES, IDC_STYLE_DETAIL}) {
                if (!GetDlgItem(dialog, id)) {
                    EndDialog(dialog, 0);
                    return TRUE;
                }
            }
            SetTimer(dialog, 1, 300, nullptr);
        }
        return TRUE;
    }
    case WM_TIMER:
        if (g_selfTest && wParam == 1) {
            KillTimer(dialog, 1);
            EndDialog(dialog, 1);
            return TRUE;
        }
        return FALSE;
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
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
    if (!selected.empty() &&
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
    if (!importSchemeDirectory(source, error, schemas)) {
        std::fwprintf(stderr, L"import failed: %ls\n", error.c_str());
        return 3;
    }
    if (!loadSchemas()) return 2;
    std::fwprintf(stdout, L"imported %zu schemas\n", schemas);
    return 0;
}
} // namespace

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
