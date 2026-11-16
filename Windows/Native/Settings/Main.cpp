#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "Version.h"
#include "resource.h"
#include "../Common/Pipe.h"
#include "../Protocol/Protocol.h"

namespace {
constexpr std::size_t kMaxSchemaBytes = 128 * 1024;
constexpr DWORD kDeployTimeoutMs = 10 * 60 * 1000;

struct SchemaInfo {
    std::string id;
    std::wstring name;
};

HINSTANCE g_instance = nullptr;
HWND g_dialog = nullptr;
std::filesystem::path g_installDirectory;
std::filesystem::path g_dataRoot;
std::vector<SchemaInfo> g_schemas;
bool g_busy = false;
bool g_selfTest = false;

void appendLog(const std::wstring& message);

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

bool loadSchemas() {
    g_schemas.clear();
    const auto resources = g_installDirectory / L"Resources" / L"Rime";
    std::error_code ec;
    for (std::filesystem::directory_iterator it(resources, ec), end; it != end && !ec; it.increment(ec)) {
        if (!it->is_regular_file(ec) || ec) continue;
        const auto name = it->path().filename().wstring();
        constexpr std::wstring_view suffix = L".schema.yaml";
        if (name.size() < suffix.size() ||
            name.compare(name.size() - suffix.size(), suffix.size(), L".schema.yaml") != 0) continue;
        SchemaInfo info;
        if (parseSchemaFile(it->path(), info)) g_schemas.push_back(std::move(info));
    }
    if (ec) return false;
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
    const auto contents = readFile(settingsPath(), 1024);
    if (!contents) return {};
    std::vector<std::string> lines;
    splitLines(*contents, lines);
    for (const auto& line : lines) {
        const std::string trimmed = trimAscii(line);
        const auto equals = trimmed.find('=');
        if (equals == std::string::npos || trimmed.compare(0, equals, "schema") != 0) continue;
        const std::string value = trimAscii(std::string_view(trimmed).substr(equals + 1));
        if (validSchemaId(value)) return value;
    }
    return {};
}

bool writeSelectedSchema(const std::string& schema) {
    return validSchemaId(schema) && writeFileAtomic(settingsPath(), "schema=" + schema + "\n");
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
    if (existing) {
        if (!replaceSchemaList(content, selected)) {
            error = L"无法更新方案列表。";
            return false;
        }
    }
    if (!writeFileAtomic(configPath, content)) {
        error = L"无法写入方案配置：" + configPath.wstring();
        return false;
    }
    if (!writeSelectedSchema(selected)) {
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
        // An existing instance may still be finishing its startup deployment.
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

void setStatus(const std::wstring& text) {
    if (g_dialog) SetDlgItemTextW(g_dialog, IDC_STATUS, text.c_str());
}

void setBusy(bool busy) {
    g_busy = busy;
    for (const int id : {IDC_APPLY, IDC_REDEPLOY, IDC_START_HOST, IDC_OPEN_USER, IDC_OPEN_LOG, IDC_OPEN_LANGUAGE}) {
        EnableWindow(GetDlgItem(g_dialog, id), !busy);
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
        SetDlgItemTextW(dialog, IDC_VERSION,
                        (L"版本 " + xmjd::win::widen(XMJD_VERSION_FULL)).c_str());
        const std::string selected = readSelectedSchema();
        int selection = 0;
        for (std::size_t index = 0; index < g_schemas.size(); ++index) {
            const std::wstring label = g_schemas[index].name + L"  (" + xmjd::win::widen(g_schemas[index].id) + L")";
            SendDlgItemMessageW(dialog, IDC_SCHEMA_LIST, LB_ADDSTRING, 0,
                                reinterpret_cast<LPARAM>(label.c_str()));
            if (g_schemas[index].id == selected) selection = static_cast<int>(index);
        }
        if (!g_schemas.empty()) {
            SendDlgItemMessageW(dialog, IDC_SCHEMA_LIST, LB_SETCURSEL, static_cast<WPARAM>(selection), 0);
        }
        refreshStatus();
        if (isElevated()) {
            setStatus(L"当前是管理员窗口；引擎和用户配置必须属于普通桌面用户。请关闭后从开始菜单以普通身份打开。");
            for (const int id : {IDC_APPLY, IDC_REDEPLOY, IDC_START_HOST}) {
                EnableWindow(GetDlgItem(dialog, id), FALSE);
            }
        }
        if (g_selfTest) {
            for (const int id : {IDC_SCHEMA_LIST, IDC_APPLY, IDC_REDEPLOY, IDC_START_HOST,
                                 IDC_OPEN_USER, IDC_OPEN_LOG, IDC_OPEN_LANGUAGE, IDC_STATUS}) {
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
    return 0;
}

int runSelect(const std::string& schema) {
    if (!loadSchemas()) return 2;
    std::wstring error;
    if (!writeConfiguration(schema, error)) return 3;
    return 0;
}
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    g_instance = instance;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
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
    } else if (argc >= 2 && _wcsicmp(argv[1], L"--self-test-ui") == 0) {
        g_selfTest = true;
    }
    LocalFree(argv);
    if (result >= 0) return result;

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
