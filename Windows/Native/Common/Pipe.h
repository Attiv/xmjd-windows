#pragma once
#include "Protocol.h"
#include <windows.h>
#include <filesystem>
#include <optional>
#include <string>

namespace xmjd::win {
inline constexpr DWORD kMaxPipeConnections = 32;
// Shared per-logon name/ACL and token verification. M0-M2 supports ordinary
// desktop tokens only; restricted/AppContainer clients are deliberately refused.
std::wstring pipeName();
std::filesystem::path modulePath(HMODULE module = nullptr);
std::filesystem::path dataRoot();
std::wstring widen(std::string_view utf8);
std::string narrow(std::wstring_view utf16);

// Server uses overlapped, byte-mode, local-only named pipes. The ACL grants
// only the current logon SID; verifyClient also checks session, integrity and
// rejects restricted tokens. Caller owns returned handles.
HANDLE createServerPipe(bool firstInstance);
bool verifyClient(HANDLE pipe);
bool connectServerPipe(HANDLE pipe, HANDLE stopEvent);
// Idle wait is separate from the fixed 50 ms deadline after the first byte.
std::optional<std::string> readFrame(HANDLE pipe, DWORD idleDeadlineMs, HANDLE stopEvent = nullptr);
bool writeFrame(HANDLE pipe, std::string_view json, DWORD deadlineMs);

class PipeClient {
public:
    PipeClient() = default;
    ~PipeClient();
    PipeClient(const PipeClient&) = delete;
    PipeClient& operator=(const PipeClient&) = delete;
    // No launch/wait/retry: CreateFile on an already-listening pipe only.
    bool connect(const std::filesystem::path& expectedHost);
    // Control requests (deploy) may legitimately exceed the 50 ms input budget.
    // Ordinary input clients never pass control=true.
    std::optional<Response> transact(const Request& request, DWORD deadlineMs = kInputDeadlineMs,
                                     bool control = false);
    void close() noexcept;
    bool connected() const noexcept { return pipe_ != INVALID_HANDLE_VALUE; }
    std::uint64_t epoch() const noexcept { return epoch_; }
private:
    HANDLE pipe_ = INVALID_HANDLE_VALUE;
    std::uint64_t epoch_ = 0;
};
} // namespace xmjd::win
