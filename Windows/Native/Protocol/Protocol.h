#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace xmjd {
inline constexpr std::uint32_t kProtocolVersion = 1;
inline constexpr std::size_t kMaxFrameBytes = 1024 * 1024;
inline constexpr std::size_t kMaxTextBytes = 128 * 1024;
inline constexpr std::size_t kMaxCandidates = 128;
inline constexpr std::uint32_t kInputDeadlineMs = 50;
inline constexpr std::uint32_t kReleaseMask = 1u << 30;

enum class Operation { Health, CreateSession, ProcessKey, SelectCandidate, Finalize, Cancel, DestroySession, Deploy, Shutdown };
enum class Status { Ok, Unavailable, Busy, Stale, UnsupportedVersion, InvalidRequest, EngineFailure };
struct Identity {
    std::uint32_t version = kProtocolVersion;
    std::uint64_t connectionEpoch = 0;
    std::uint64_t sessionId = 0;
    std::uint64_t contextEpoch = 0;
    std::uint64_t sequence = 0;
    bool operator==(const Identity& other) const;
};
struct Request {
    Identity id;
    Operation operation = Operation::Health;
    std::uint32_t keycode = 0;
    std::uint32_t modifiers = 0;
    std::uint32_t candidateIndex = 0; // Index on the current Rime page, never an invented selection key.
    std::uint64_t snapshotRevision = 0;
    bool convert = true;
};
struct Candidate {
    std::string text;
    std::string comment;
    std::string label;
};
struct Snapshot {
    std::string commitText;
    std::string rawInput;
    std::string preedit;
    std::size_t caretUtf8Offset = 0;
    std::vector<Candidate> candidates;
    std::uint32_t page = 0;
    std::uint32_t pageSize = 0;
    std::uint32_t highlightedIndex = 0;
    bool lastPage = true;
    bool asciiMode = false;
    std::uint64_t revision = 0;
};
struct Response {
    Identity id;
    Status status = Status::Ok;
    bool handled = false;
    Snapshot snapshot;
};

// All decoders reject malformed UTF-8, wrong types, negative/out-of-range integers,
// duplicate fields, excessive nesting and operation-specific limits. No exceptions escape.
bool validUtf8(std::string_view text) noexcept;
std::u16string utf8ToUtf16(std::string_view text);
std::size_t utf16Caret(std::string_view text, std::size_t byteOffset);
std::string encodeRequest(const Request& request);
std::string encodeResponse(const Response& response);
std::optional<Request> decodeRequest(std::string_view json) noexcept;
std::optional<Response> decodeResponse(std::string_view json) noexcept;
std::vector<std::uint8_t> frame(std::string_view utf8);
std::optional<std::uint32_t> frameLength(const std::uint8_t* header, std::size_t size) noexcept;
const char* statusName(Status status) noexcept;

// The broker owns engine lifetime. Every method is invoked under its serialized
// engine gate; disconnect destroys only this connection's sessions.
class Engine {
public:
    virtual ~Engine() = default;
    virtual bool ready() const = 0;
    virtual std::uint64_t createSession() = 0;
    virtual void destroySession(std::uint64_t session) = 0;
    virtual bool processKey(std::uint64_t session, std::uint32_t key, std::uint32_t modifiers) = 0;
    virtual bool selectCandidate(std::uint64_t session, std::uint32_t pageIndex) = 0;
    virtual void finalize(std::uint64_t session, bool convert) = 0;
    virtual void cancel(std::uint64_t session) = 0;
    // 默认一次性取出 commit；drainCommit=false 返回空 commitText，保留待提交内容。
    virtual Snapshot snapshot(std::uint64_t session, bool drainCommit = true) = 0;
};
} // namespace xmjd
