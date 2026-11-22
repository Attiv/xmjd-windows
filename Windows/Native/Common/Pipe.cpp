#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "Pipe.h"
#include "PipeAccessPolicy.h"

#include <aclapi.h>
#include <sddl.h>
#include <shlobj.h>
#include <algorithm>
#include <cstring>
#include <cwctype>
#include <exception>
#include <limits>
#include <utility>
#include <vector>

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")

namespace xmjd::win {
namespace {
constexpr wchar_t kPipePrefix[] = L"\\\\.\\pipe\\XMJD\\";
constexpr wchar_t kPipeSuffix[] = L"\\broker-v1";
// GENERIC_WRITE includes FILE_CREATE_PIPE_INSTANCE for named pipes. Clients
// need data/attribute I/O, not the right to create another server instance.
constexpr DWORD kPipeAccess = (FILE_GENERIC_READ | FILE_GENERIC_WRITE) & ~FILE_CREATE_PIPE_INSTANCE;
constexpr std::uint64_t kJsonSafeInteger = 9007199254740991ULL;

struct TokenDetails {
    std::vector<std::uint8_t> user;
    std::vector<std::uint8_t> logon;
    DWORD session = 0;
    detail::PipeTokenContext context;
};

class HandleGuard {
public:
    explicit HandleGuard(HANDLE handle = INVALID_HANDLE_VALUE) : handle_(handle) {}
    ~HandleGuard() { reset(); }
    HandleGuard(const HandleGuard&) = delete;
    HandleGuard& operator=(const HandleGuard&) = delete;
    HANDLE get() const noexcept { return handle_; }
    HANDLE release() noexcept { return std::exchange(handle_, INVALID_HANDLE_VALUE); }
    void reset(HANDLE handle = INVALID_HANDLE_VALUE) noexcept {
        if (handle_ != INVALID_HANDLE_VALUE && handle_ != nullptr) CloseHandle(handle_);
        handle_ = handle;
    }
    explicit operator bool() const noexcept { return handle_ != INVALID_HANDLE_VALUE && handle_ != nullptr; }
private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};

class LocalDeadline {
public:
    explicit LocalDeadline(DWORD durationMs, HANDLE stopEvent = nullptr)
        : end_(GetTickCount64() + durationMs), stopEvent_(stopEvent), infinite_(durationMs == INFINITE) {}
    HANDLE stopEvent() const noexcept { return stopEvent_; }
    DWORD remaining() const noexcept {
        if (stopEvent_ && WaitForSingleObject(stopEvent_, 0) != WAIT_TIMEOUT) return 0;
        if (infinite_) return INFINITE;
        const ULONGLONG now = GetTickCount64();
        if (now >= end_) return 0;
        const ULONGLONG left = end_ - now;
        return left > std::numeric_limits<DWORD>::max() ? std::numeric_limits<DWORD>::max() : static_cast<DWORD>(left);
    }
private:
    ULONGLONG end_;
    HANDLE stopEvent_;
    bool infinite_;
};

bool readTokenField(HANDLE token, TOKEN_INFORMATION_CLASS klass, std::vector<std::uint8_t>& bytes) {
    DWORD required = 0;
    GetTokenInformation(token, klass, nullptr, 0, &required);
    if (required == 0) return false;
    bytes.resize(required);
    return GetTokenInformation(token, klass, bytes.data(), required, &required) != FALSE;
}

bool readTokenDetails(HANDLE token, TokenDetails& details) {
    std::vector<std::uint8_t> user;
    std::vector<std::uint8_t> logon;
    if (!readTokenField(token, TokenUser, user) || !readTokenField(token, TokenLogonSid, logon)) return false;
    const auto* userInfo = reinterpret_cast<const TOKEN_USER*>(user.data());
    const auto* logonInfo = reinterpret_cast<const TOKEN_GROUPS*>(logon.data());
    if (logonInfo->GroupCount == 0 || !IsValidSid(userInfo->User.Sid) || !IsValidSid(logonInfo->Groups[0].Sid)) return false;
    DWORD session = 0;
    DWORD size = sizeof(session);
    if (!GetTokenInformation(token, TokenSessionId, &session, size, &size)) return false;
    TOKEN_MANDATORY_LABEL* label = nullptr;
    std::vector<std::uint8_t> integrity;
    if (!readTokenField(token, TokenIntegrityLevel, integrity)) return false;
    label = reinterpret_cast<TOKEN_MANDATORY_LABEL*>(integrity.data());
    if (!label->Label.Sid || *GetSidSubAuthorityCount(label->Label.Sid) == 0) return false;
    const DWORD integrityRid = *GetSidSubAuthority(label->Label.Sid, *GetSidSubAuthorityCount(label->Label.Sid) - 1);
    // TokenIsAppContainer can return zero for an identification-level token.
    // Read the package SID itself so that such clients cannot gain control commands.
    std::vector<std::uint8_t> package;
    if (!readTokenField(token, TokenAppContainerSid, package) ||
        package.size() < sizeof(TOKEN_APPCONTAINER_INFORMATION)) return false;
    const auto packageSid = reinterpret_cast<const TOKEN_APPCONTAINER_INFORMATION*>(package.data())->TokenAppContainer;
    if (packageSid && !IsValidSid(packageSid)) return false;
    TOKEN_ELEVATION elevation{};
    size = sizeof(elevation);
    if (!GetTokenInformation(token, TokenElevation, &elevation, size, &size)) return false;
    const auto* userSid = reinterpret_cast<const std::uint8_t*>(userInfo->User.Sid);
    const auto* logonSid = reinterpret_cast<const std::uint8_t*>(logonInfo->Groups[0].Sid);
    details.user.assign(userSid, userSid + GetLengthSid(userInfo->User.Sid));
    details.logon.assign(logonSid, logonSid + GetLengthSid(logonInfo->Groups[0].Sid));
    details.session = session;
    details.context = {integrityRid, IsTokenRestricted(token) != FALSE,
                       packageSid != nullptr, elevation.TokenIsElevated != FALSE};
    return true;
}

bool currentTokenDetails(TokenDetails& details) {
    HANDLE raw = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw)) return false;
    HandleGuard token(raw);
    return readTokenDetails(token.get(), details);
}

PSID sidPointer(const std::vector<std::uint8_t>& bytes) noexcept {
    return reinterpret_cast<PSID>(const_cast<std::uint8_t*>(bytes.data()));
}

std::optional<PipeClientKind> classifyClient(const TokenDetails& server, const TokenDetails& client) {
    const bool sameLogon = server.session != 0 && server.session == client.session &&
        EqualSid(sidPointer(server.user), sidPointer(client.user)) &&
        EqualSid(sidPointer(server.logon), sidPointer(client.logon));
    return detail::classifyPipeClient(server.context, client.context, sameLogon);
}

std::wstring currentLogonSidString() {
    TokenDetails details;
    if (!currentTokenDetails(details)) return {};
    LPWSTR sid = nullptr;
    const PSID logonSid = reinterpret_cast<PSID>(details.logon.data());
    if (!ConvertSidToStringSidW(logonSid, &sid)) return {};
    std::wstring result(sid);
    LocalFree(sid);
    return result;
}

bool makeSecurityAttributes(SECURITY_ATTRIBUTES& attributes, PSECURITY_DESCRIPTOR& descriptor) {
    TokenDetails details;
    if (!currentTokenDetails(details)) return false;
    if (details.context.restricted || details.context.appContainer ||
        details.context.integrity < SECURITY_MANDATORY_MEDIUM_RID) return false;
    LPWSTR rawSid = nullptr;
    if (!ConvertSidToStringSidW(sidPointer(details.logon), &rawSid)) return false;
    const std::wstring logonSid(rawSid);
    LocalFree(rawSid);
    // The Host's logon SID can create server instances. Packages get only client
    // I/O; verifyClient still requires the same user, session and logon SID.
    // OWNER RIGHTS prevents the implicit owner WRITE_DAC grant. Low integrity
    // is needed in addition to the package DACL: DACL access cannot bypass MIC.
    wchar_t clientRights[16]{};
    swprintf_s(clientRights, L"0x%08lX", static_cast<unsigned long>(kPipeAccess));
    const std::wstring sddl = L"D:P(A;;GRGW;;;" + logonSid + L")"
        L"(A;;" + clientRights + L";;;AC)(A;;RC;;;OW)S:(ML;;NW;;;LW)";
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) return false;
    attributes.nLength = sizeof(attributes);
    attributes.lpSecurityDescriptor = descriptor;
    attributes.bInheritHandle = FALSE;
    return true;
}

std::wstring fullPath(std::wstring path) {
    DWORD required = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
    if (required == 0) return {};
    std::wstring result(static_cast<std::size_t>(required) + 1, L'\0');
    DWORD written = GetFullPathNameW(path.c_str(), static_cast<DWORD>(result.size()), result.data(), nullptr);
    if (written == 0 || written >= result.size()) return {};
    result.resize(written);
    for (wchar_t& c : result) {
        if (c == L'/') c = L'\\';
        c = static_cast<wchar_t>(towlower(c));
    }
    return result;
}

std::wstring processImagePath(HANDLE process) {
    std::wstring result(512, L'\0');
    for (;;) {
        DWORD length = static_cast<DWORD>(result.size());
        if (QueryFullProcessImageNameW(process, 0, result.data(), &length)) {
            result.resize(length);
            return fullPath(std::move(result));
        }
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || result.size() >= 32768) return {};
        result.resize(result.size() * 2);
    }
}

bool isProtectedInstallHost(const std::wstring& path) {
    // Read the native Program Files path even for a WOW64 client. Do not take
    // it from the game's environment. Elevated TIPs never trust a dev install.
    wchar_t programFiles[MAX_PATH]{};
    DWORD bytes = sizeof(programFiles);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion",
                     L"ProgramFilesDir", RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY,
                     nullptr, programFiles, &bytes) != ERROR_SUCCESS) return false;
    const auto root = fullPath((std::filesystem::path(programFiles) / L"XMJD").wstring());
    const std::filesystem::path exe(path);
    const auto install = exe.parent_path();
    if (root.empty() || install.parent_path().wstring() != root || exe.filename() != L"xmjdhost.exe") return false;
    for (const auto& component : {std::filesystem::path(root), install, exe}) {
        const DWORD attributes = GetFileAttributesW(component.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
    }
    return true;
}

bool verifyPeer(HANDLE pipe, const std::filesystem::path& expectedHost, bool& inputOnly,
                const char*& failureStage, DWORD& failureCode) {
    const auto fail = [&](const char* stage, DWORD code) {
        failureStage = stage;
        failureCode = code == ERROR_SUCCESS ? ERROR_GEN_FAILURE : code;
        return false;
    };
    ULONG processId = 0;
    if (!GetNamedPipeServerProcessId(pipe, &processId)) return fail("peer-server-pid", GetLastError());
    if (processId == 0) return fail("peer-server-pid", ERROR_INVALID_DATA);
    SetLastError(ERROR_SUCCESS);
    HandleGuard process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId));
    if (!process) return fail("peer-open-process", GetLastError());
    HandleGuard token;
    HANDLE rawToken = nullptr;
    SetLastError(ERROR_SUCCESS);
    if (!OpenProcessToken(process.get(), TOKEN_QUERY, &rawToken)) return fail("peer-open-token", GetLastError());
    token.reset(rawToken);
    TokenDetails peer;
    TokenDetails current;
    SetLastError(ERROR_SUCCESS);
    if (!readTokenDetails(token.get(), peer)) return fail("peer-server-token", GetLastError());
    SetLastError(ERROR_SUCCESS);
    if (!currentTokenDetails(current)) return fail("peer-current-token", GetLastError());
    const auto kind = classifyClient(peer, current);
    if (!kind) return fail("peer-token-boundary", ERROR_ACCESS_DENIED);
    SetLastError(ERROR_SUCCESS);
    const std::wstring actual = processImagePath(process.get());
    if (actual.empty()) return fail("peer-image-query", GetLastError());
    const std::wstring expected = fullPath(expectedHost.wstring());
    if (expected.empty()) return fail("peer-expected-path", GetLastError());
    if (actual != expected) return fail("peer-image-mismatch", ERROR_ACCESS_DENIED);
    if (*kind == PipeClientKind::Elevated && !isProtectedInstallHost(actual))
        return fail("peer-install-path", ERROR_ACCESS_DENIED);
    inputOnly = *kind == PipeClientKind::Elevated;
    failureStage = "connected";
    failureCode = ERROR_SUCCESS;
    return true;
}

DWORD grantQueryAccess(HANDLE object, PSID sid, DWORD access) noexcept {
    PACL original = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    DWORD error = GetSecurityInfo(object, SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION,
                                  nullptr, nullptr, &original, nullptr, &descriptor);
    if (error == ERROR_SUCCESS) {
        if (!original) {
            error = ERROR_INVALID_ACL;
        } else {
            EXPLICIT_ACCESSW entry{};
            entry.grfAccessPermissions = access;
            entry.grfAccessMode = GRANT_ACCESS;
            BuildTrusteeWithSidW(&entry.Trustee, sid);
            PACL updated = nullptr;
            error = SetEntriesInAclW(1, &entry, original, &updated);
            if (error == ERROR_SUCCESS) {
                error = SetSecurityInfo(object, SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION,
                                        nullptr, nullptr, updated, nullptr);
            }
            if (updated) LocalFree(updated);
        }
    }
    if (descriptor) LocalFree(descriptor);
    return error;
}

bool waitForOverlapped(HANDLE pipe, OVERLAPPED& overlapped, LocalDeadline& deadline, DWORD& transferred) {
    const DWORD remaining = deadline.remaining();
    if (remaining == 0) {
        CancelIoEx(pipe, &overlapped);
        WaitForSingleObject(overlapped.hEvent, INFINITE);
        GetOverlappedResult(pipe, &overlapped, &transferred, FALSE);
        return false;
    }
    HANDLE waits[2] = {overlapped.hEvent, deadline.stopEvent()};
    const DWORD wait = WaitForMultipleObjects(deadline.stopEvent() ? 2 : 1, waits, FALSE, remaining);
    if (wait != WAIT_OBJECT_0) {
        CancelIoEx(pipe, &overlapped);
        WaitForSingleObject(overlapped.hEvent, INFINITE);
        GetOverlappedResult(pipe, &overlapped, &transferred, FALSE);
        return false;
    }
    return GetOverlappedResult(pipe, &overlapped, &transferred, FALSE) != FALSE;
}

bool readExactUntil(HANDLE pipe, std::uint8_t* buffer, std::size_t size, LocalDeadline& deadline) {
    std::size_t offset = 0;
    while (offset < size) {
        if (deadline.remaining() == 0) return false;
        HandleGuard event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!event) return false;
        OVERLAPPED overlapped{};
        overlapped.hEvent = event.get();
        DWORD transferred = 0;
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(size - offset, std::numeric_limits<DWORD>::max()));
        if (ReadFile(pipe, buffer + offset, chunk, nullptr, &overlapped)) {
            if (GetOverlappedResult(pipe, &overlapped, &transferred, FALSE) == FALSE) return false;
        } else if (GetLastError() == ERROR_IO_PENDING) {
            if (!waitForOverlapped(pipe, overlapped, deadline, transferred)) return false;
        } else {
            return false;
        }
        if (transferred == 0) return false;
        offset += transferred;
    }
    return true;
}

bool writeExactUntil(HANDLE pipe, const std::uint8_t* buffer, std::size_t size, LocalDeadline& deadline) {
    std::size_t offset = 0;
    while (offset < size) {
        if (deadline.remaining() == 0) return false;
        HandleGuard event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!event) return false;
        OVERLAPPED overlapped{};
        overlapped.hEvent = event.get();
        DWORD transferred = 0;
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(size - offset, std::numeric_limits<DWORD>::max()));
        if (WriteFile(pipe, buffer + offset, chunk, nullptr, &overlapped)) {
            if (GetOverlappedResult(pipe, &overlapped, &transferred, FALSE) == FALSE) return false;
        } else if (GetLastError() == ERROR_IO_PENDING) {
            if (!waitForOverlapped(pipe, overlapped, deadline, transferred)) return false;
        } else {
            return false;
        }
        if (transferred == 0) return false;
        offset += transferred;
    }
    return true;
}

std::optional<std::string> readFrameUntil(HANDLE pipe, LocalDeadline& deadline) {
    std::uint8_t header[4]{};
    if (!readExactUntil(pipe, header, sizeof(header), deadline)) return std::nullopt;
    const auto length = xmjd::frameLength(header, sizeof(header));
    if (!length) return std::nullopt;
    std::string body(*length, '\0');
    if (!readExactUntil(pipe, reinterpret_cast<std::uint8_t*>(body.data()), body.size(), deadline)) return std::nullopt;
    if (!xmjd::validUtf8(body)) return std::nullopt;
    return body;
}

bool writeFrameUntil(HANDLE pipe, std::string_view json, LocalDeadline& deadline) {
    const std::vector<std::uint8_t> bytes = xmjd::frame(json);
    return !bytes.empty() && writeExactUntil(pipe, bytes.data(), bytes.size(), deadline);
}

std::uint64_t newEpoch() noexcept {
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    std::uint64_t value = static_cast<std::uint64_t>(counter.QuadPart) ^ (static_cast<std::uint64_t>(GetCurrentProcessId()) << 32) ^ GetTickCount64();
    value &= kJsonSafeInteger;
    return value == 0 ? 1 : value;
}

bool responseMatches(const Request& request, const Response& response) {
    if (response.id.version != request.id.version || response.id.connectionEpoch != request.id.connectionEpoch ||
        response.id.contextEpoch != request.id.contextEpoch || response.id.sequence != request.id.sequence) return false;
    if (response.appearance && (request.operation != Operation::CreateSession || !request.wantsAppearance)) return false;
    if (request.operation == Operation::CreateSession) {
        if (response.status == Status::Ok && response.handled) return response.id.sessionId != 0;
        return response.id.sessionId == 0;
    }
    return response.id.sessionId == request.id.sessionId;
}
} // namespace

std::wstring pipeName() {
    const std::wstring sid = currentLogonSidString();
    if (sid.empty()) return {};
    return std::wstring(kPipePrefix) + sid + kPipeSuffix;
}

std::filesystem::path modulePath(HMODULE module) {
    std::wstring result(512, L'\0');
    for (;;) {
        const DWORD length = GetModuleFileNameW(module, result.data(), static_cast<DWORD>(result.size()));
        if (length == 0) return {};
        if (length < result.size() - 1) {
            result.resize(length);
            return result;
        }
        if (result.size() >= 32768) return {};
        result.resize(result.size() * 2);
    }
}

std::filesystem::path dataRoot() {
    PWSTR raw = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &raw)) || !raw) return {};
    std::filesystem::path result(raw);
    CoTaskMemFree(raw);
    result /= L"XMJD";
    return result;
}

std::wstring widen(std::string_view utf8) {
    if (utf8.empty()) return {};
    if (utf8.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) return {};
    const int inputLength = static_cast<int>(utf8.size());
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), inputLength, nullptr, 0);
    if (length <= 0) return {};
    std::wstring result(length, L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), inputLength, result.data(), length) != length) return {};
    return result;
}

std::string narrow(std::wstring_view utf16) {
    if (utf16.empty()) return {};
    if (utf16.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) return {};
    const int inputLength = static_cast<int>(utf16.size());
    const int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, utf16.data(), inputLength, nullptr, 0, nullptr, nullptr);
    if (length <= 0) return {};
    std::string result(length, '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, utf16.data(), inputLength, result.data(), length, nullptr, nullptr) != length) return {};
    return result;
}

HANDLE createServerPipe(bool firstInstance, const std::wstring& name) {
    if (name.empty()) return INVALID_HANDLE_VALUE;
    SECURITY_ATTRIBUTES attributes{};
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!makeSecurityAttributes(attributes, descriptor)) return INVALID_HANDLE_VALUE;
    const DWORD flags = PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | (firstInstance ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0);
    HANDLE pipe = CreateNamedPipeW(name.c_str(), flags, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                                   kMaxPipeConnections, static_cast<DWORD>(kMaxFrameBytes + 4), static_cast<DWORD>(kMaxFrameBytes + 4), 0, &attributes);
    LocalFree(descriptor);
    return pipe == INVALID_HANDLE_VALUE ? INVALID_HANDLE_VALUE : pipe;
}

DWORD allowPipeServerIdentityQuery(PSID appContainerSid) noexcept {
    const SID_IDENTIFIER_AUTHORITY packageAuthority = SECURITY_APP_PACKAGE_AUTHORITY;
    if (!appContainerSid || !IsValidSid(appContainerSid) ||
        std::memcmp(GetSidIdentifierAuthority(appContainerSid), &packageAuthority, sizeof(packageAuthority)) != 0 ||
        *GetSidSubAuthorityCount(appContainerSid) != SECURITY_APP_PACKAGE_RID_COUNT ||
        *GetSidSubAuthority(appContainerSid, 0) != SECURITY_APP_PACKAGE_BASE_RID) return ERROR_INVALID_SID;

    const HANDLE process = GetCurrentProcess();
    HANDLE raw = nullptr;
    if (!OpenProcessToken(process, READ_CONTROL | WRITE_DAC, &raw)) return GetLastError();
    HandleGuard token(raw);
    const DWORD tokenResult = grantQueryAccess(token.get(), appContainerSid, TOKEN_QUERY);
    if (tokenResult != ERROR_SUCCESS) return tokenResult;
    return grantQueryAccess(process, appContainerSid, PROCESS_QUERY_LIMITED_INFORMATION);
}

std::optional<VerifiedClient> verifyClient(HANDLE pipe) {
    if (pipe == INVALID_HANDLE_VALUE || pipe == nullptr) return std::nullopt;
    TokenDetails current;
    if (!currentTokenDetails(current) || !ImpersonateNamedPipeClient(pipe)) return std::nullopt;
    HANDLE rawToken = nullptr;
    const bool opened = OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &rawToken) != FALSE;
    HandleGuard token(rawToken);
    TokenDetails client;
    bool read = false;
    try {
        read = opened && readTokenDetails(token.get(), client);
    } catch (...) {
        if (!RevertToSelf()) std::terminate();
        return std::nullopt;
    }
    // 身份验证失败不允许线程继续以客户端身份处理后续引擎操作。
    if (!RevertToSelf()) std::terminate();
    if (!read) return std::nullopt;
    const auto kind = classifyClient(current, client);
    if (!kind) return std::nullopt;
    return VerifiedClient{*kind};
}

bool connectServerPipe(HANDLE pipe, HANDLE stopEvent) {
    if (pipe == INVALID_HANDLE_VALUE || pipe == nullptr) return false;
    HandleGuard event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event) return false;
    OVERLAPPED overlapped{};
    overlapped.hEvent = event.get();
    if (ConnectNamedPipe(pipe, &overlapped)) return true;
    const DWORD error = GetLastError();
    if (error == ERROR_PIPE_CONNECTED) return true;
    if (error != ERROR_IO_PENDING) return false;
    HANDLE waits[2] = {event.get(), stopEvent};
    const DWORD count = stopEvent && stopEvent != INVALID_HANDLE_VALUE ? 2 : 1;
    const DWORD result = WaitForMultipleObjects(count, waits, FALSE, INFINITE);
    if (result == WAIT_OBJECT_0) {
        DWORD transferred = 0;
        return GetOverlappedResult(pipe, &overlapped, &transferred, FALSE) != FALSE;
    }
    CancelIoEx(pipe, &overlapped);
    WaitForSingleObject(event.get(), INFINITE);
    DWORD transferred = 0;
    GetOverlappedResult(pipe, &overlapped, &transferred, FALSE);
    DisconnectNamedPipe(pipe);
    return false;
}

std::optional<std::string> readFrame(HANDLE pipe, DWORD idleDeadlineMs, HANDLE stopEvent) {
    if (pipe == INVALID_HANDLE_VALUE || pipe == nullptr) return std::nullopt;
    std::uint8_t header[4]{};
    LocalDeadline idle(idleDeadlineMs, stopEvent);
    if (!readExactUntil(pipe, header, 1, idle)) return std::nullopt;
    // An established client may remain idle, but once a frame starts it must
    // finish promptly. A partial header/body cannot occupy a worker forever.
    LocalDeadline payload(kInputDeadlineMs, stopEvent);
    if (!readExactUntil(pipe, header + 1, sizeof(header) - 1, payload)) return std::nullopt;
    const auto length = xmjd::frameLength(header, sizeof(header));
    if (!length) return std::nullopt;
    std::string body(*length, '\0');
    if (!readExactUntil(pipe, reinterpret_cast<std::uint8_t*>(body.data()), body.size(), payload) ||
        !xmjd::validUtf8(body)) return std::nullopt;
    return body;
}

bool writeFrame(HANDLE pipe, std::string_view json, DWORD deadlineMs) {
    if (pipe == INVALID_HANDLE_VALUE || pipe == nullptr) return false;
    LocalDeadline deadline(deadlineMs);
    return writeFrameUntil(pipe, json, deadline);
}

bool writeFinalControlFrame(HANDLE pipe, std::string_view json, HANDLE stopEvent) {
    LocalDeadline writeDeadline(kInputDeadlineMs, stopEvent);
    if (pipe == INVALID_HANDLE_VALUE || pipe == nullptr || !writeFrameUntil(pipe, json, writeDeadline)) return false;
    // DisconnectNamedPipe would discard the unread part of the reply. Settings
    // closes its pipe after reading it; a bounded read observes that EOF without
    // the indefinite wait of FlushFileBuffers. Do not accept another request.
    constexpr DWORD kClientCloseDeadlineMs = 2000;
    LocalDeadline closeDeadline(kClientCloseDeadlineMs, stopEvent);
    std::uint8_t unexpectedByte = 0;
    (void)readExactUntil(pipe, &unexpectedByte, 1, closeDeadline);
    return true;
}

PipeClient::~PipeClient() {
    close();
}

bool PipeClient::connect(const std::filesystem::path& expectedHost, const std::wstring& name) {
    close();
    connectFailureStage_ = "pipe-name";
    connectFailureCode_ = ERROR_SUCCESS;
    if (name.empty()) {
        connectFailureCode_ = GetLastError();
        return false;
    }
    connectFailureStage_ = "host-path";
    if (expectedHost.empty() || !expectedHost.is_absolute()) {
        connectFailureCode_ = ERROR_INVALID_PARAMETER;
        return false;
    }
    connectFailureStage_ = "pipe-open";
    // The server needs to identify the client, never to act with its privileges.
    constexpr DWORD flags = FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION;
    HANDLE raw = CreateFileW(name.c_str(), kPipeAccess, 0, nullptr, OPEN_EXISTING, flags, nullptr);
    if (raw == INVALID_HANDLE_VALUE) {
        connectFailureCode_ = GetLastError();
        return false;
    }
    HandleGuard candidate(raw);
    connectFailureStage_ = "peer-verify";
    if (!verifyPeer(candidate.get(), expectedHost, inputOnly_, connectFailureStage_, connectFailureCode_)) {
        return false;
    }
    pipe_ = candidate.release();
    epoch_ = newEpoch();
    connectFailureStage_ = "connected";
    connectFailureCode_ = ERROR_SUCCESS;
    return true;
}

std::optional<Response> PipeClient::transact(const Request& request, DWORD deadlineMs, bool control) {
    if (!connected() || request.id.connectionEpoch != epoch_ || request.id.version != kProtocolVersion) return std::nullopt;
    const std::string encoded = encodeRequest(request);
    if (encoded.empty()) {
        close();
        return std::nullopt;
    }
    constexpr DWORD kControlDeadlineMs = 10 * 60 * 1000;
    LocalDeadline deadline(std::min<DWORD>(deadlineMs,
        control ? kControlDeadlineMs : static_cast<DWORD>(kInputDeadlineMs)));
    if (!writeFrameUntil(pipe_, encoded, deadline)) {
        close();
        return std::nullopt;
    }
    const auto body = readFrameUntil(pipe_, deadline);
    if (!body) {
        close();
        return std::nullopt;
    }
    const auto response = decodeResponse(*body);
    if (!response || !responseMatches(request, *response)) {
        close();
        return std::nullopt;
    }
    return response;
}

void PipeClient::close() noexcept {
    if (pipe_ != INVALID_HANDLE_VALUE) {
        CancelIoEx(pipe_, nullptr);
        CloseHandle(pipe_);
        pipe_ = INVALID_HANDLE_VALUE;
    }
    epoch_ = 0;
    inputOnly_ = false;
}
} // namespace xmjd::win

#endif
