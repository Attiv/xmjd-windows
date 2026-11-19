#pragma once
#include "Protocol.h"
#include <windows.h>
#include <filesystem>
#include <optional>
#include <string>

namespace xmjd::win {
inline constexpr DWORD kMaxPipeConnections = 32;
// Shared per-logon name/ACL and token verification. AppContainer clients may
// connect across integrity levels, with the same user/session/logon checks.
std::wstring pipeName();
std::filesystem::path modulePath(HMODULE module = nullptr);
std::filesystem::path dataRoot();
std::wstring widen(std::string_view utf8);
std::string narrow(std::wstring_view utf16);

// Server uses overlapped, byte-mode, local-only named pipes with a Low label.
// The logon SID gets server access, packages get client I/O only; verifyClient
// checks user/session/logon, rejects restricted tokens and checks desktop IL.
// Caller owns returned handles.
HANDLE createServerPipe(bool firstInstance, const std::wstring& name = pipeName());
// Allow a specific AppContainer to authenticate this server by inspecting its
// image path and token identity, without granting write or impersonation access.
DWORD allowPipeServerIdentityQuery(PSID appContainerSid) noexcept;
// Engine lifecycle requests are reserved for verified desktop clients.
struct VerifiedClient {
    bool appContainer;
};
std::optional<VerifiedClient> verifyClient(HANDLE pipe);
bool connectServerPipe(HANDLE pipe, HANDLE stopEvent);
// Idle wait is separate from the fixed 50 ms deadline after the first byte.
std::optional<std::string> readFrame(HANDLE pipe, DWORD idleDeadlineMs, HANDLE stopEvent = nullptr);
bool writeFrame(HANDLE pipe, std::string_view json, DWORD deadlineMs);
// Finish a one-shot control request. Keep the reply readable until the client
// closes its handle, but do not let a stalled client hold a worker indefinitely.
bool writeFinalControlFrame(HANDLE pipe, std::string_view json, HANDLE stopEvent);

class PipeClient {
public:
    PipeClient() = default;
    ~PipeClient();
    PipeClient(const PipeClient&) = delete;
    PipeClient& operator=(const PipeClient&) = delete;
    // No launch/wait/retry: CreateFile on an already-listening pipe only.
    bool connect(const std::filesystem::path& expectedHost, const std::wstring& name = pipeName());
    const char* connectFailureStage() const noexcept { return connectFailureStage_; }
    DWORD connectFailureCode() const noexcept { return connectFailureCode_; }
    // Control requests (deploy) may legitimately exceed the 50 ms input budget;
    // Ordinary input clients never pass control=true.
    std::optional<Response> transact(const Request& request, DWORD deadlineMs = kInputDeadlineMs,
                                     bool control = false);
    void close() noexcept;
    bool connected() const noexcept { return pipe_ != INVALID_HANDLE_VALUE; }
    std::uint64_t epoch() const noexcept { return epoch_; }
private:
    HANDLE pipe_ = INVALID_HANDLE_VALUE;
    std::uint64_t epoch_ = 0;
    const char* connectFailureStage_ = "not-attempted";
    DWORD connectFailureCode_ = ERROR_SUCCESS;
};
} // namespace xmjd::win
