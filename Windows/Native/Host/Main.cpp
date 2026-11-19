#include "RimeRuntime.h"
#include "../Common/Pipe.h"
#include "../Common/CandidatePainter.h"
#include "../Protocol/Broker.h"

#include <array>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>
#include <windows.h>
#include <sddl.h>
#include <userenv.h>

static_assert(sizeof(void*) == 8, "XMJDHost 只支持 x64 构建");
#if !defined(_M_X64) && !defined(__x86_64__)
#error XMJDHost 只支持 x64 架构
#endif

namespace {
constexpr std::size_t kMaxConnections = xmjd::win::kMaxPipeConnections;
constexpr int kExitUsage = 2;
constexpr int kExitIdentity = 3;
constexpr int kExitAlreadyRunning = 4;
constexpr int kExitDataLock = 5;
constexpr int kExitRuntime = 6;
constexpr int kExitPipe = 7;

std::atomic<HANDLE> g_stopEvent{nullptr};

const char* operationName(xmjd::Operation operation) noexcept {
    switch (operation) {
    case xmjd::Operation::CreateSession: return "create-session";
    case xmjd::Operation::ProcessKey: return "process-key";
    default: return "other";
    }
}

void logInputResponse(HANDLE pipe, const xmjd::win::VerifiedClient& client,
                      const xmjd::Request& request, const xmjd::Response& response) noexcept {
    if (request.operation != xmjd::Operation::CreateSession && request.operation != xmjd::Operation::ProcessKey) return;
    ULONG clientId = 0;
    if (!GetNamedPipeClientProcessId(pipe, &clientId)) clientId = 0;
    char line[320]{};
    std::snprintf(line, sizeof(line),
                  "XMJDHost IPC pid=%lu appcontainer=%u op=%s seq=%llu status=%s handled=%u commit=%zuB preedit=%zuB input=%zuB ascii=%u\n",
                  static_cast<unsigned long>(clientId), client.appContainer ? 1u : 0u,
                  operationName(request.operation), static_cast<unsigned long long>(request.id.sequence),
                  xmjd::statusName(response.status), response.handled ? 1u : 0u,
                  response.snapshot.commitText.size(), response.snapshot.preedit.size(),
                  response.snapshot.rawInput.size(), response.snapshot.asciiMode ? 1u : 0u);
    OutputDebugStringA(line);
}

void allowWindowsShellToVerifyHost() noexcept {
    // Only the built-in shell packages can query the Host. They still pass
    // the same user, logon, session and image-path checks as desktop TIPs.
    constexpr const wchar_t* shellPackages[] = {
        L"MicrosoftWindows.Client.CBS_cw5n1h2txyewy",
        L"Microsoft.Windows.Search_cw5n1h2txyewy",
        L"Microsoft.Windows.StartMenuExperienceHost_cw5n1h2txyewy",
    };
    for (const auto* package : shellPackages) {
        PSID sid = nullptr;
        const HRESULT hr = DeriveAppContainerSidFromAppContainerName(package, &sid);
        if (FAILED(hr)) {
            std::cerr << "shell-package-sid-error " << std::hex << hr << std::dec << '\n';
            continue;
        }
        const DWORD error = xmjd::win::allowPipeServerIdentityQuery(sid);
        FreeSid(sid);
        if (error != ERROR_SUCCESS) std::cerr << "shell-peer-query-error " << error << '\n';
    }
}

// 登录项启动时系统会为控制台程序新建一个窗口并一直挂着。只有本进程独占
// 控制台（没有 cmd/PowerShell 同时附着）时才隐藏，避免把用户自己在终端里
// 运行的宿主窗口一起藏掉。
void hideOwnedConsoleWindow() {
    const HWND console = GetConsoleWindow();
    if (!console || !IsWindowVisible(console)) return;
    DWORD processes[2]{};
    const DWORD count = GetConsoleProcessList(processes, static_cast<DWORD>(std::size(processes)));
    if (count == 1) ShowWindow(console, SW_HIDE);
}

BOOL WINAPI controlHandler(DWORD control) {
    if (control == CTRL_C_EVENT || control == CTRL_BREAK_EVENT || control == CTRL_CLOSE_EVENT ||
        control == CTRL_LOGOFF_EVENT || control == CTRL_SHUTDOWN_EVENT) {
        if (HANDLE stopEvent = g_stopEvent.load()) SetEvent(stopEvent);
        return TRUE;
    }
    return FALSE;
}

class UniqueHandle {
public:
    UniqueHandle() = default;
    explicit UniqueHandle(HANDLE value) : value_(value) {}
    ~UniqueHandle() { reset(); }
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;
    UniqueHandle(UniqueHandle&& other) noexcept : value_(other.release()) {}
    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) reset(other.release());
        return *this;
    }
    HANDLE get() const { return value_; }
    HANDLE release() { const HANDLE value = value_; value_ = nullptr; return value; }
    void reset(HANDLE value = nullptr) {
        if (value_ && value_ != INVALID_HANDLE_VALUE) CloseHandle(value_);
        value_ = value;
    }
    explicit operator bool() const { return value_ && value_ != INVALID_HANDLE_VALUE; }
private:
    HANDLE value_ = nullptr;
};

std::wstring currentLogonMutexName() {
    UniqueHandle token;
    HANDLE rawToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &rawToken)) return {};
    token.reset(rawToken);

    DWORD userSize = 0;
    GetTokenInformation(token.get(), TokenUser, nullptr, 0, &userSize);
    if (!userSize) return {};
    std::vector<std::uint8_t> userBuffer(userSize);
    if (!GetTokenInformation(token.get(), TokenUser, userBuffer.data(), userSize, &userSize)) return {};
    const auto* user = reinterpret_cast<const TOKEN_USER*>(userBuffer.data());
    LPWSTR sidText = nullptr;
    if (!ConvertSidToStringSidW(user->User.Sid, &sidText)) return {};
    std::wstring sid(sidText);
    LocalFree(sidText);

    DWORD statsSize = 0;
    GetTokenInformation(token.get(), TokenStatistics, nullptr, 0, &statsSize);
    if (!statsSize) return {};
    std::vector<std::uint8_t> statsBuffer(statsSize);
    if (!GetTokenInformation(token.get(), TokenStatistics, statsBuffer.data(), statsSize, &statsSize)) return {};
    const auto* stats = reinterpret_cast<const TOKEN_STATISTICS*>(statsBuffer.data());
    DWORD session = 0;
    DWORD sessionSize = 0;
    if (!GetTokenInformation(token.get(), TokenSessionId, &session, sizeof(session), &sessionSize)) return {};

    return L"Local\\XMJDHost-" + sid + L"-" + std::to_wstring(stats->AuthenticationId.HighPart) +
           L"-" + std::to_wstring(stats->AuthenticationId.LowPart) + L"-" + std::to_wstring(session);
}

bool isOrdinaryInteractiveUser() {
    HANDLE rawToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &rawToken)) return false;
    UniqueHandle token(rawToken);

    TOKEN_ELEVATION elevation{};
    DWORD returned = 0;
    if (!GetTokenInformation(token.get(), TokenElevation, &elevation, sizeof(elevation), &returned) || elevation.TokenIsElevated) return false;
    DWORD appContainer = 0;
    if (!GetTokenInformation(token.get(), TokenIsAppContainer, &appContainer, sizeof(appContainer), &returned) || appContainer ||
        IsTokenRestricted(token.get())) return false;
    DWORD session = 0;
    if (!GetTokenInformation(token.get(), TokenSessionId, &session, sizeof(session), &returned) || session == 0) return false;
    DWORD integritySize = 0;
    GetTokenInformation(token.get(), TokenIntegrityLevel, nullptr, 0, &integritySize);
    if (!integritySize) return false;
    std::vector<std::uint8_t> integrityBuffer(integritySize);
    if (!GetTokenInformation(token.get(), TokenIntegrityLevel, integrityBuffer.data(), integritySize, &integritySize)) return false;
    const auto* label = reinterpret_cast<const TOKEN_MANDATORY_LABEL*>(integrityBuffer.data());
    if (!IsValidSid(label->Label.Sid) || *GetSidSubAuthorityCount(label->Label.Sid) == 0) return false;
    const DWORD integrityRid = *GetSidSubAuthority(label->Label.Sid,
        static_cast<DWORD>(*GetSidSubAuthorityCount(label->Label.Sid) - 1));
    if (integrityRid < SECURITY_MANDATORY_MEDIUM_RID || integrityRid >= SECURITY_MANDATORY_HIGH_RID) return false;

    DWORD logonSize = 0;
    GetTokenInformation(token.get(), TokenLogonSid, nullptr, 0, &logonSize);
    if (!logonSize) return false;
    std::vector<std::uint8_t> logonBuffer(logonSize);
    if (!GetTokenInformation(token.get(), TokenLogonSid, logonBuffer.data(), logonSize, &logonSize)) return false;
    const auto* logon = reinterpret_cast<const TOKEN_GROUPS*>(logonBuffer.data());
    if (logon->GroupCount != 1 || !IsValidSid(logon->Groups[0].Sid)) return false;

    DWORD userSize = 0;
    GetTokenInformation(token.get(), TokenUser, nullptr, 0, &userSize);
    if (!userSize) return false;
    std::vector<std::uint8_t> userBuffer(userSize);
    if (!GetTokenInformation(token.get(), TokenUser, userBuffer.data(), userSize, &userSize)) return false;
    PSID user = reinterpret_cast<const TOKEN_USER*>(userBuffer.data())->User.Sid;
    return IsValidSid(user) && !IsWellKnownSid(user, WinLocalSystemSid) &&
           !IsWellKnownSid(user, WinLocalServiceSid) && !IsWellKnownSid(user, WinNetworkServiceSid);
}

class DataRootLock {
public:
    DataRootLock() = default;
    DataRootLock(const DataRootLock&) = delete;
    DataRootLock& operator=(const DataRootLock&) = delete;

    bool acquire(const std::filesystem::path& root) {
        if (locked_ || root.empty() || !root.is_absolute()) return false;
        std::error_code ec;
        std::filesystem::create_directories(root, ec);
        if (ec) return false;
        directory_.reset(CreateFileW(root.c_str(), FILE_READ_ATTRIBUTES,
                                     FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                     FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        BY_HANDLE_FILE_INFORMATION directoryInfo{};
        if (!directory_ || !GetFileInformationByHandle(directory_.get(), &directoryInfo) ||
            !(directoryInfo.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
            (directoryInfo.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;

        handle_.reset(CreateFileW((root / L"XMJD.host.lock").c_str(), GENERIC_READ | GENERIC_WRITE,
                                  FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                                  FILE_ATTRIBUTE_HIDDEN | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        BY_HANDLE_FILE_INFORMATION fileInfo{};
        if (!handle_ || !GetFileInformationByHandle(handle_.get(), &fileInfo) ||
            (fileInfo.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
            fileInfo.nNumberOfLinks != 1) return false;
        OVERLAPPED overlapped{};
        if (!LockFileEx(handle_.get(), LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
                        0, MAXDWORD, MAXDWORD, &overlapped)) return false;
        locked_ = true;
        for (const auto* name : {L"SharedData", L"UserData", L"Staging", L"Backups", L"Logs"}) {
            const auto managedDirectory = root / name;
            std::filesystem::create_directories(managedDirectory, ec);
            if (ec || !retainDirectory(managedDirectory) || !rejectReparseTree(managedDirectory)) return false;
        }
        return true;
    }
    ~DataRootLock() {
        if (locked_) {
            OVERLAPPED overlapped{};
            UnlockFileEx(handle_.get(), 0, MAXDWORD, MAXDWORD, &overlapped);
        }
    }
private:
    bool retainDirectory(const std::filesystem::path& path) {
        UniqueHandle directory(CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
                                            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        BY_HANDLE_FILE_INFORMATION info{};
        if (!directory || !GetFileInformationByHandle(directory.get(), &info) ||
            !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
            (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
        managedDirectories_.push_back(std::move(directory));
        return true;
    }

    bool rejectReparseTree(const std::filesystem::path& path) {
        std::error_code ec;
        for (std::filesystem::recursive_directory_iterator it(path, ec), end;
             it != end && !ec; it.increment(ec)) {
            const DWORD attributes = GetFileAttributesW(it->path().c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
        }
        return !ec;
    }

    UniqueHandle directory_;
    UniqueHandle handle_;
    std::vector<UniqueHandle> managedDirectories_;
    bool locked_ = false;
};

struct ServerState {
    HANDLE stopEvent = nullptr;
    HANDLE workersDoneEvent = nullptr;
    HANDLE firstPipe = INVALID_HANDLE_VALUE;
    std::atomic<std::size_t> workerCount{0};
    std::mutex engineGate;
    std::mutex pipesMutex;
    std::atomic<std::uint64_t> deploymentEpoch{0};
    std::array<HANDLE, kMaxConnections> activePipes{};
    xmjd::Engine& engine;
    std::function<bool(int&)> deploy;

    explicit ServerState(xmjd::Engine& value) : engine(value) { activePipes.fill(INVALID_HANDLE_VALUE); }

    void setPipe(std::size_t index, HANDLE pipe) {
        std::lock_guard<std::mutex> lock(pipesMutex);
        activePipes[index] = pipe;
    }

    void cancelPipes() {
        std::lock_guard<std::mutex> lock(pipesMutex);
        for (HANDLE pipe : activePipes) {
            if (pipe != INVALID_HANDLE_VALUE) CancelIoEx(pipe, nullptr);
        }
    }

    // Deployment destroys every engine session. Reset input connections so the
    // TIPs recreate sessions against the newly active scheme instead of
    // retaining dead engine IDs.
    void resetInputConnections(std::size_t controlWorker) {
        std::lock_guard<std::mutex> lock(pipesMutex);
        // Do not let another deployment cancel this worker's final reply.
        activePipes[controlWorker] = INVALID_HANDLE_VALUE;
        deploymentEpoch.fetch_add(1, std::memory_order_acq_rel);
        for (HANDLE pipe : activePipes) {
            if (pipe != INVALID_HANDLE_VALUE) CancelIoEx(pipe, nullptr);
        }
    }
};

void connectionWorker(ServerState& state, std::size_t workerIndex) noexcept {
    struct Completion {
        ServerState& state;
        ~Completion() {
            if (state.workerCount.fetch_sub(1) == 1) SetEvent(state.workersDoneEvent);
        }
    } completion{state};

    UniqueHandle pipe;
    std::unique_ptr<xmjd::BrokerConnection> broker;
    try {
        pipe.reset(workerIndex == 0 ? state.firstPipe : xmjd::win::createServerPipe(false));
        while (pipe && WaitForSingleObject(state.stopEvent, 0) != WAIT_OBJECT_0) {
            const std::uint64_t connectionDeploymentEpoch = state.deploymentEpoch.load(std::memory_order_acquire);
            state.setPipe(workerIndex, pipe.get());
            const bool connected = xmjd::win::connectServerPipe(pipe.get(), state.stopEvent);
            if (connected && connectionDeploymentEpoch == state.deploymentEpoch.load(std::memory_order_acquire)) {
                broker = std::make_unique<xmjd::BrokerConnection>(state.engine);
                bool firstFrame = true;
                for (;;) {
                    if (WaitForSingleObject(state.stopEvent, 0) == WAIT_OBJECT_0 ||
                        connectionDeploymentEpoch != state.deploymentEpoch.load(std::memory_order_acquire)) break;
                    // Reclaim uninitialized clients without expiring an ordinary
                    // established input session while the user is not typing.
                    auto frame = xmjd::win::readFrame(pipe.get(), firstFrame ? 5000 : INFINITE, state.stopEvent);
                    if (!frame || connectionDeploymentEpoch != state.deploymentEpoch.load(std::memory_order_acquire)) break;
                    const auto verifiedClient = xmjd::win::verifyClient(pipe.get());
                    if (!verifiedClient) break;
                    firstFrame = false;
                    auto request = xmjd::decodeRequest(*frame);
                    if (!request) break;

                    xmjd::Response response;
                    bool finalControlReply = false;
                    bool shutdownRequested = false;
                    {
                        std::unique_lock<std::mutex> gate(state.engineGate, std::try_to_lock);
                        if (connectionDeploymentEpoch != state.deploymentEpoch.load(std::memory_order_acquire)) {
                            response.id = request->id;
                            response.id.version = xmjd::kProtocolVersion;
                            response.status = xmjd::Status::Stale;
                        } else if (gate.owns_lock() && request->operation == xmjd::Operation::Deploy) {
                            response.id = request->id;
                            response.id.version = xmjd::kProtocolVersion;
                            if (request->id.version != xmjd::kProtocolVersion) {
                                response.status = xmjd::Status::UnsupportedVersion;
                            } else if (request->id.connectionEpoch == 0 || request->id.sessionId != 0 ||
                                       request->id.contextEpoch != 0) {
                                response.status = xmjd::Status::InvalidRequest;
                            } else if (verifiedClient->appContainer) {
                                response.status = xmjd::Status::InvalidRequest;
                            } else if (!state.deploy) {
                                response.status = xmjd::Status::Unavailable;
                            } else {
                                int deployError = 0;
                                const bool deployed = state.deploy(deployError);
                                response.status = deployed ? xmjd::Status::Ok : xmjd::Status::EngineFailure;
                                response.handled = deployed;
                                response.diagnosticCode = static_cast<std::uint32_t>(deployError);
                                // Rime invalidates sessions before attempting the deploy,
                                // even if rebuilding or loading the scheme later fails.
                                state.resetInputConnections(workerIndex);
                                finalControlReply = true;
                            }
                        } else if (gate.owns_lock() && request->operation == xmjd::Operation::Shutdown) {
                            // 方案来源换了之后必须整个重启——Rime 的 shared_data_dir 只在
                            // initialize 时生效，就地重新部署不会换目录。
                            response.id = request->id;
                            response.id.version = xmjd::kProtocolVersion;
                            if (verifiedClient->appContainer || request->id.version != xmjd::kProtocolVersion ||
                                request->id.connectionEpoch == 0 || request->id.sessionId != 0 ||
                                request->id.contextEpoch != 0) {
                                response.status = xmjd::Status::InvalidRequest;
                            } else {
                                response.status = xmjd::Status::Ok;
                                response.handled = true;
                                state.setPipe(workerIndex, INVALID_HANDLE_VALUE);
                                finalControlReply = true;
                                shutdownRequested = true;
                            }
                        } else if (gate.owns_lock()) {
                            response = broker->dispatch(*request);
                        } else {
                            response.id = request->id;
                            response.id.version = xmjd::kProtocolVersion;
                            response.status = xmjd::Status::Busy;
                        }
                    }
                    if (request->wantsAppearance && response.status == xmjd::Status::Ok &&
                        response.handled && response.id.sessionId != 0) {
                        // AppContainer TIPs cannot see the normal user's active.json.
                        // Publish only the validated skin, over the authenticated session.
                        try {
                            auto json = xmjd::serializeCandidateStyle(
                                xmjd::loadActiveCandidateStyle(xmjd::win::dataRoot()), false);
                            if (!json.empty() && json.size() <= xmjd::kMaxCandidateAppearanceBytes) {
                                response.appearance = xmjd::Response::CandidateAppearance{
                                    std::move(json), xmjd::windowsCandidateDarkMode()};
                            }
                        } catch (...) {
                            // Appearance is optional; failure must not reject a valid session.
                        }
                    }
                    logInputResponse(pipe.get(), *verifiedClient, *request, response);
                    const std::string encoded = xmjd::encodeResponse(response);
                    if (finalControlReply) {
                        const bool sent = xmjd::win::writeFinalControlFrame(pipe.get(), encoded, state.stopEvent);
                        if (sent && shutdownRequested) SetEvent(state.stopEvent);
                        break;
                    }
                    if (!xmjd::win::writeFrame(pipe.get(), encoded, xmjd::kInputDeadlineMs)) break;
                }
                std::lock_guard<std::mutex> gate(state.engineGate);
                broker.reset();
            }
            state.setPipe(workerIndex, INVALID_HANDLE_VALUE);
            DisconnectNamedPipe(pipe.get());
            pipe.reset();
            if (WaitForSingleObject(state.stopEvent, 0) == WAIT_OBJECT_0) break;
            pipe.reset(xmjd::win::createServerPipe(false));
        }
    } catch (...) {
        if (broker) {
            std::lock_guard<std::mutex> gate(state.engineGate);
            broker.reset();
        }
    }
    state.setPipe(workerIndex, INVALID_HANDLE_VALUE);
    if (pipe) DisconnectNamedPipe(pipe.get());
}

int healthCheck(const std::filesystem::path& hostPath) {
    xmjd::win::PipeClient client;
    if (!client.connect(hostPath)) {
        std::cout << "unavailable\n";
        return 1;
    }
    xmjd::Request request;
    request.operation = xmjd::Operation::Health;
    request.id.connectionEpoch = client.epoch();
    request.id.sequence = 1;
    const auto response = client.transact(request);
    if (!response) {
        std::cout << "unavailable\n";
        return 1;
    }
    std::cout << xmjd::statusName(response->status) << '\n';
    return response->status == xmjd::Status::Ok ? 0 : 1;
}

// 诊断用：把一串按键送进正在运行的 Host，打印每个按键后的上屏内容、
// 预编辑和候选。只走本地管道，不创建第二个引擎，也不影响输入法会话。
int probeKeys(const wchar_t* keys) {
    SetConsoleOutputCP(CP_UTF8);
    xmjd::win::PipeClient client;
    const auto hostPath = xmjd::win::modulePath();
    if (hostPath.empty() || !client.connect(hostPath)) {
        std::cout << "host-unavailable\n";
        return 1;
    }
    constexpr std::uint64_t kProbeEpoch = 1;
    std::uint64_t sequence = 0;
    xmjd::Request create;
    create.operation = xmjd::Operation::CreateSession;
    create.id.connectionEpoch = client.epoch();
    create.id.contextEpoch = kProbeEpoch;
    create.id.sequence = ++sequence;
    auto created = client.transact(create, 30000, true);
    if (!created || created->status != xmjd::Status::Ok || created->id.sessionId == 0) {
        std::cout << "create-session-failed\n";
        return 1;
    }
    const std::uint64_t session = created->id.sessionId;

    auto print = [](const char* label, const xmjd::Response& response) {
        std::cout << label << " handled=" << (response.handled ? 1 : 0)
                  << " status=" << xmjd::statusName(response.status)
                  << " commit=\"" << response.snapshot.commitText << "\""
                  << " preedit=\"" << response.snapshot.preedit << "\""
                  << " input=\"" << response.snapshot.rawInput << "\""
                  << " candidates=" << response.snapshot.candidates.size();
        for (std::size_t i = 0; i < response.snapshot.candidates.size() && i < 5; ++i) {
            std::cout << " [" << i << ']' << response.snapshot.candidates[i].text;
        }
        std::cout << '\n';
    };

    auto sendKey = [&](std::uint32_t keycode, const char* label) {
        xmjd::Request request;
        request.operation = xmjd::Operation::ProcessKey;
        request.id.connectionEpoch = client.epoch();
        request.id.sessionId = session;
        request.id.contextEpoch = kProbeEpoch;
        request.id.sequence = ++sequence;
        request.keycode = keycode;
        auto response = client.transact(request, 30000, true);
        if (!response) {
            std::cout << label << " transact-failed\n";
            return false;
        }
        print(label, *response);
        return true;
    };

    for (const wchar_t* p = keys; *p; ++p) {
        if (*p < 0x20 || *p > 0x7e) continue;
        char label[16]{};
        std::snprintf(label, sizeof(label), "key '%c'", static_cast<char>(*p));
        if (!sendKey(static_cast<std::uint32_t>(*p), label)) break;
    }
    sendKey(0x20, "space");

    xmjd::Request destroy;
    destroy.operation = xmjd::Operation::DestroySession;
    destroy.id.connectionEpoch = client.epoch();
    destroy.id.sessionId = session;
    destroy.id.contextEpoch = kProbeEpoch;
    destroy.id.sequence = ++sequence;
    (void)client.transact(destroy, 5000);
    return 0;
}
} // namespace

int runHost(int argc, wchar_t** argv) {
    if (argc == 3 && std::wstring_view(argv[1]) == L"--probe") {
        return probeKeys(argv[2]);
    }
    if (argc == 2 && std::wstring_view(argv[1]) == L"--health") {
        return healthCheck(xmjd::win::modulePath());
    }
    const bool deploy = argc == 2 && std::wstring_view(argv[1]) == L"--deploy";
    if (argc > 2 || (argc == 2 && !deploy)) {
        std::cerr << "usage-error\n";
        return kExitUsage;
    }
    if (!isOrdinaryInteractiveUser()) {
        std::cerr << "identity-error\n";
        return kExitIdentity;
    }

    const std::wstring mutexName = currentLogonMutexName();
    if (mutexName.empty()) {
        std::cerr << "identity-error\n";
        return kExitIdentity;
    }
    UniqueHandle instanceMutex(CreateMutexW(nullptr, FALSE, mutexName.c_str()));
    if (!instanceMutex || GetLastError() == ERROR_ALREADY_EXISTS) {
        std::cerr << "already-running\n";
        return kExitAlreadyRunning;
    }

    const auto executablePath = xmjd::win::modulePath();
    if (executablePath.empty() || !executablePath.is_absolute()) {
        std::cerr << "runtime-error 10\n";
        return kExitRuntime;
    }
    const auto executableDirectory = executablePath.parent_path();
    const auto root = xmjd::win::dataRoot();
    DataRootLock dataLock;
    if (root.empty() || !dataLock.acquire(root)) {
        std::cerr << "data-root-busy\n";
        return kExitDataLock;
    }

    xmjd::win::RimeRuntime runtime(executableDirectory, root);
    int runtimeError = 0;
    if (!runtime.initialize(deploy, runtimeError)) {
        std::cerr << "runtime-error " << runtimeError << '\n';
        return kExitRuntime;
    }
    if (deploy) {
        std::cout << "deploy-ok\n";
        return 0;
    }

    UniqueHandle stopEvent(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    UniqueHandle workersDoneEvent(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!stopEvent || !workersDoneEvent) {
        std::cerr << "pipe-error\n";
        return kExitPipe;
    }
    g_stopEvent = stopEvent.get();
    if (!SetConsoleCtrlHandler(controlHandler, TRUE)) {
        g_stopEvent = nullptr;
        std::cerr << "pipe-error\n";
        return kExitPipe;
    }

    allowWindowsShellToVerifyHost();
    UniqueHandle firstPipe(xmjd::win::createServerPipe(true));
    if (!firstPipe) {
        SetConsoleCtrlHandler(controlHandler, FALSE);
        g_stopEvent = nullptr;
        std::cerr << "pipe-error\n";
        return kExitPipe;
    }

    ServerState state(runtime);
    state.deploy = [&runtime](int& error) { return runtime.redeploy(error); };
    state.stopEvent = stopEvent.get();
    state.workersDoneEvent = workersDoneEvent.get();
    state.firstPipe = firstPipe.get();
    // 启动期间保留一个计数，避免工作线程先退出而误报全部完成。
    state.workerCount = 1;
    std::vector<std::thread> workers;
    try {
        workers.reserve(kMaxConnections);
        for (std::size_t i = 0; i < kMaxConnections; ++i) {
            state.workerCount.fetch_add(1);
            try {
                workers.emplace_back(connectionWorker, std::ref(state), i);
            } catch (...) {
                state.workerCount.fetch_sub(1);
                throw;
            }
            if (i == 0) (void)firstPipe.release();
        }
    } catch (...) {
        SetEvent(stopEvent.get());
        state.cancelPipes();
        for (auto& worker : workers) if (worker.joinable()) worker.join();
        SetConsoleCtrlHandler(controlHandler, FALSE);
        g_stopEvent = nullptr;
        std::cerr << "pipe-error\n";
        return kExitPipe;
    }
    if (state.workerCount.fetch_sub(1) == 1) SetEvent(workersDoneEvent.get());

    HANDLE waits[2] = {stopEvent.get(), workersDoneEvent.get()};
    WaitForMultipleObjects(2, waits, FALSE, INFINITE);
    SetEvent(stopEvent.get());
    state.cancelPipes();
    for (auto& worker : workers) if (worker.joinable()) worker.join();
    SetConsoleCtrlHandler(controlHandler, FALSE);
    g_stopEvent = nullptr;
    return 0;
}

int wmain(int argc, wchar_t** argv) {
    // GUI 子系统不分配控制台；只有从终端启动时才借用父进程的那个，
    // 这样诊断命令的输出还在，登录自启也不会闪窗口。
    // 注意：stdout 已经被重定向成管道/文件时不能抢，否则 --probe 这类
    // "… | head" 的调用会一个字都拿不到。
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        const HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
        const DWORD type = out ? GetFileType(out) : FILE_TYPE_UNKNOWN;
        if (type != FILE_TYPE_PIPE && type != FILE_TYPE_DISK) {
            FILE* stream = nullptr;
            freopen_s(&stream, "CONOUT$", "w", stdout);
            freopen_s(&stream, "CONOUT$", "w", stderr);
        }
    }
    hideOwnedConsoleWindow();
    if (!SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS)) {
        std::cerr << "runtime-error 18\n";
        return kExitRuntime;
    }
    try {
        return runHost(argc, argv);
    } catch (...) {
        std::cerr << "runtime-error\n";
        return kExitRuntime;
    }
}
