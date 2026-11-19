#include "Protocol.h"

#include <algorithm>
#include <charconv>
#include <limits>
#include <string>
#include <utility>

namespace xmjd {
namespace {
constexpr std::uint64_t kJsonSafeInteger = 9007199254740991ULL;
constexpr std::size_t kMaxFieldNameBytes = 64;
constexpr unsigned kMaxJsonDepth = 8;

const char* operationName(Operation operation) noexcept {
    switch (operation) {
    case Operation::Health: return "health";
    case Operation::CreateSession: return "createSession";
    case Operation::ProcessKey: return "processKey";
    case Operation::SelectCandidate: return "selectCandidate";
    case Operation::Finalize: return "finalize";
    case Operation::Cancel: return "cancel";
    case Operation::DestroySession: return "destroySession";
    case Operation::Deploy: return "deploy";
    case Operation::Shutdown: return "shutdown";
    }
    return nullptr;
}

std::optional<Operation> parseOperation(std::string_view name) noexcept {
    if (name == "health") return Operation::Health;
    if (name == "createSession") return Operation::CreateSession;
    if (name == "processKey") return Operation::ProcessKey;
    if (name == "selectCandidate") return Operation::SelectCandidate;
    if (name == "finalize") return Operation::Finalize;
    if (name == "cancel") return Operation::Cancel;
    if (name == "destroySession") return Operation::DestroySession;
    if (name == "deploy") return Operation::Deploy;
    if (name == "shutdown") return Operation::Shutdown;
    return std::nullopt;
}

std::optional<Status> parseStatus(std::string_view name) noexcept {
    if (name == "ok") return Status::Ok;
    if (name == "unavailable") return Status::Unavailable;
    if (name == "busy") return Status::Busy;
    if (name == "stale") return Status::Stale;
    if (name == "unsupportedVersion") return Status::UnsupportedVersion;
    if (name == "invalidRequest") return Status::InvalidRequest;
    if (name == "engineFailure") return Status::EngineFailure;
    return std::nullopt;
}

bool safeInteger(std::uint64_t value) noexcept {
    return value <= kJsonSafeInteger;
}

void appendJsonString(std::string& output, std::string_view value) {
    output.push_back('"');
    for (unsigned char byte : value) {
        switch (byte) {
        case '"': output += "\\\""; break;
        case '\\': output += "\\\\"; break;
        case '\b': output += "\\b"; break;
        case '\f': output += "\\f"; break;
        case '\n': output += "\\n"; break;
        case '\r': output += "\\r"; break;
        case '\t': output += "\\t"; break;
        default:
            if (byte < 0x20) {
                static constexpr char digits[] = "0123456789abcdef";
                output += "\\u00";
                output.push_back(digits[(byte >> 4) & 0x0f]);
                output.push_back(digits[byte & 0x0f]);
            } else {
                output.push_back(static_cast<char>(byte));
            }
            break;
        }
    }
    output.push_back('"');
}

void appendNumber(std::string& output, std::uint64_t value) {
    char buffer[32]{};
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
    output.append(buffer, result.ptr);
}

bool textForWire(std::string_view value) noexcept {
    return value.size() <= kMaxTextBytes && validUtf8(value);
}

bool caretBoundary(std::string_view value, std::size_t offset) noexcept {
    if (offset > value.size() || !validUtf8(value)) return false;
    if (offset == 0 || offset == value.size()) return true;
    return (static_cast<unsigned char>(value[offset]) & 0xc0) != 0x80;
}

class Parser {
public:
    explicit Parser(std::string_view input) : input_(input) {}

    bool end() {
        whitespace();
        return position_ == input_.size();
    }

    bool objectStart() {
        whitespace();
        if (position_ >= input_.size() || input_[position_] != '{' || depth_ >= kMaxJsonDepth) return false;
        ++position_;
        ++depth_;
        whitespace();
        return true;
    }

    bool objectEnd(bool& ended) {
        whitespace();
        if (position_ < input_.size() && input_[position_] == '}') {
            ++position_;
            --depth_;
            ended = true;
            return true;
        }
        ended = false;
        return true;
    }

    bool arrayStart() {
        whitespace();
        if (position_ >= input_.size() || input_[position_] != '[' || depth_ >= kMaxJsonDepth) return false;
        ++position_;
        ++depth_;
        whitespace();
        return true;
    }

    bool arrayEnd(bool& ended) {
        whitespace();
        if (position_ < input_.size() && input_[position_] == ']') {
            ++position_;
            --depth_;
            ended = true;
            return true;
        }
        ended = false;
        return true;
    }

    bool comma() {
        whitespace();
        if (position_ >= input_.size() || input_[position_] != ',') return false;
        ++position_;
        whitespace();
        return position_ < input_.size() && input_[position_] != '}' && input_[position_] != ']';
    }

    bool colon() {
        whitespace();
        if (position_ >= input_.size() || input_[position_] != ':') return false;
        ++position_;
        whitespace();
        return true;
    }

    bool string(std::string& result, std::size_t maximum) {
        result.clear();
        whitespace();
        if (position_ >= input_.size() || input_[position_] != '"') return false;
        ++position_;
        while (position_ < input_.size()) {
            const unsigned char byte = static_cast<unsigned char>(input_[position_++]);
            if (byte == '"') return validUtf8(result);
            if (byte < 0x20) return false;
            if (byte != '\\') {
                if (result.size() >= maximum) return false;
                result.push_back(static_cast<char>(byte));
                continue;
            }
            if (position_ >= input_.size()) return false;
            const char escape = input_[position_++];
            switch (escape) {
            case '"': result.push_back('"'); break;
            case '\\': result.push_back('\\'); break;
            case '/': result.push_back('/'); break;
            case 'b': result.push_back('\b'); break;
            case 'f': result.push_back('\f'); break;
            case 'n': result.push_back('\n'); break;
            case 'r': result.push_back('\r'); break;
            case 't': result.push_back('\t'); break;
            case 'u': {
                std::uint32_t code = 0;
                if (!hexCodeUnit(code)) return false;
                if (code >= 0xd800 && code <= 0xdbff) {
                    if (position_ + 5 > input_.size() || input_[position_] != '\\' || input_[position_ + 1] != 'u') return false;
                    position_ += 2;
                    std::uint32_t low = 0;
                    if (!hexCodeUnit(low) || low < 0xdc00 || low > 0xdfff) return false;
                    code = 0x10000 + ((code - 0xd800) << 10) + (low - 0xdc00);
                } else if (code >= 0xdc00 && code <= 0xdfff) {
                    return false;
                }
                if (!appendCodePoint(result, code, maximum)) return false;
                break;
            }
            default: return false;
            }
            if (result.size() > maximum) return false;
        }
        return false;
    }

    bool unsignedInteger(std::uint64_t& result) {
        whitespace();
        if (position_ >= input_.size() || input_[position_] < '0' || input_[position_] > '9') return false;
        if (input_[position_] == '0') {
            ++position_;
            if (position_ < input_.size() && input_[position_] >= '0' && input_[position_] <= '9') return false;
            result = 0;
            return true;
        }
        result = 0;
        while (position_ < input_.size() && input_[position_] >= '0' && input_[position_] <= '9') {
            const std::uint64_t digit = static_cast<std::uint64_t>(input_[position_++] - '0');
            if (result > (kJsonSafeInteger - digit) / 10) return false;
            result = result * 10 + digit;
        }
        if (position_ < input_.size() && (input_[position_] == '.' || input_[position_] == 'e' || input_[position_] == 'E')) return false;
        return true;
    }

    bool boolean(bool& result) {
        whitespace();
        if (input_.substr(position_, 4) == "true") {
            position_ += 4;
            result = true;
            return true;
        }
        if (input_.substr(position_, 5) == "false") {
            position_ += 5;
            result = false;
            return true;
        }
        return false;
    }

private:
    bool hexCodeUnit(std::uint32_t& result) {
        if (position_ + 4 > input_.size()) return false;
        result = 0;
        for (unsigned i = 0; i != 4; ++i) {
            const unsigned char c = static_cast<unsigned char>(input_[position_++]);
            result <<= 4;
            if (c >= '0' && c <= '9') result |= c - '0';
            else if (c >= 'a' && c <= 'f') result |= c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') result |= c - 'A' + 10;
            else return false;
        }
        return true;
    }

    static bool appendCodePoint(std::string& output, std::uint32_t code, std::size_t maximum) {
        if (code <= 0x7f) {
            if (output.size() + 1 > maximum) return false;
            output.push_back(static_cast<char>(code));
        } else if (code <= 0x7ff) {
            if (output.size() + 2 > maximum) return false;
            output.push_back(static_cast<char>(0xc0 | (code >> 6)));
            output.push_back(static_cast<char>(0x80 | (code & 0x3f)));
        } else if (code <= 0xffff) {
            if (output.size() + 3 > maximum) return false;
            output.push_back(static_cast<char>(0xe0 | (code >> 12)));
            output.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3f)));
            output.push_back(static_cast<char>(0x80 | (code & 0x3f)));
        } else if (code <= 0x10ffff) {
            if (output.size() + 4 > maximum) return false;
            output.push_back(static_cast<char>(0xf0 | (code >> 18)));
            output.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3f)));
            output.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3f)));
            output.push_back(static_cast<char>(0x80 | (code & 0x3f)));
        } else {
            return false;
        }
        return true;
    }

    void whitespace() {
        while (position_ < input_.size()) {
            const char c = input_[position_];
            if (c != ' ' && c != '\t' && c != '\r' && c != '\n') break;
            ++position_;
        }
    }

    std::string_view input_;
    std::size_t position_ = 0;
    unsigned depth_ = 0;
};

bool parseIdentityField(Parser& parser, std::string_view key, Identity& id, unsigned& seen) {
    auto parse = [&](std::uint64_t& target, unsigned bit) {
        if ((seen & bit) != 0) return false;
        if (!parser.unsignedInteger(target)) return false;
        seen |= bit;
        return true;
    };
    if (key == "version") {
        std::uint64_t value = 0;
        if (!parse(value, 1u << 0) || value > std::numeric_limits<std::uint32_t>::max()) return false;
        id.version = static_cast<std::uint32_t>(value);
        return true;
    }
    if (key == "connectionEpoch") return parse(id.connectionEpoch, 1u << 1);
    if (key == "sessionId") return parse(id.sessionId, 1u << 2);
    if (key == "contextEpoch") return parse(id.contextEpoch, 1u << 3);
    if (key == "sequence") return parse(id.sequence, 1u << 4);
    return false;
}

struct RequestPayload {
    std::uint32_t keycode = 0;
    std::uint32_t modifiers = 0;
    std::uint32_t candidateIndex = 0;
    std::uint64_t snapshotRevision = 0;
    bool convert = true;
    bool hasKeycode = false;
    bool hasModifiers = false;
    bool hasCandidateIndex = false;
    bool hasSnapshotRevision = false;
    bool hasConvert = false;
    bool wantsAppearance = false;
    bool hasWantsAppearance = false;
};

bool parseRequestPayload(Parser& parser, RequestPayload& payload) {
    if (!parser.objectStart()) return false;
    unsigned seen = 0;
    bool ended = false;
    if (!parser.objectEnd(ended)) return false;
    if (!ended) {
        while (true) {
            std::string key;
            if (!parser.string(key, kMaxFieldNameBytes) || !parser.colon()) return false;
            if (key == "keycode") {
                if ((seen & (1u << 0)) != 0) return false;
                std::uint64_t value = 0;
                if (!parser.unsignedInteger(value) || value > std::numeric_limits<std::uint32_t>::max()) return false;
                payload.keycode = static_cast<std::uint32_t>(value);
                payload.hasKeycode = true;
                seen |= 1u << 0;
            } else if (key == "modifiers") {
                if ((seen & (1u << 1)) != 0) return false;
                std::uint64_t value = 0;
                if (!parser.unsignedInteger(value) || value > std::numeric_limits<std::uint32_t>::max()) return false;
                payload.modifiers = static_cast<std::uint32_t>(value);
                payload.hasModifiers = true;
                seen |= 1u << 1;
            } else if (key == "candidateIndex") {
                if ((seen & (1u << 2)) != 0) return false;
                std::uint64_t value = 0;
                if (!parser.unsignedInteger(value) || value > std::numeric_limits<std::uint32_t>::max()) return false;
                payload.candidateIndex = static_cast<std::uint32_t>(value);
                payload.hasCandidateIndex = true;
                seen |= 1u << 2;
            } else if (key == "snapshotRevision") {
                if ((seen & (1u << 3)) != 0 || !parser.unsignedInteger(payload.snapshotRevision)) return false;
                payload.hasSnapshotRevision = true;
                seen |= 1u << 3;
            } else if (key == "convert") {
                if ((seen & (1u << 4)) != 0 || !parser.boolean(payload.convert)) return false;
                payload.hasConvert = true;
                seen |= 1u << 4;
            } else if (key == "wantsAppearance") {
                if (payload.hasWantsAppearance || !parser.boolean(payload.wantsAppearance)) return false;
                payload.hasWantsAppearance = true;
            } else {
                return false;
            }
            if (!parser.objectEnd(ended)) return false;
            if (ended) break;
            if (!parser.comma()) return false;
        }
    }
    return true;
}

bool validateRequestPayload(Operation operation, const RequestPayload& payload) {
    if (payload.hasWantsAppearance && operation != Operation::CreateSession) return false;
    switch (operation) {
    case Operation::ProcessKey:
        return payload.hasKeycode && payload.hasModifiers && !payload.hasCandidateIndex && !payload.hasSnapshotRevision && !payload.hasConvert;
    case Operation::SelectCandidate:
        return !payload.hasKeycode && !payload.hasModifiers && payload.hasCandidateIndex && payload.candidateIndex < kMaxCandidates &&
               payload.hasSnapshotRevision && !payload.hasConvert;
    case Operation::Finalize:
        return !payload.hasKeycode && !payload.hasModifiers && !payload.hasCandidateIndex && !payload.hasSnapshotRevision && payload.hasConvert;
    case Operation::Health:
    case Operation::CreateSession:
    case Operation::Cancel:
    case Operation::DestroySession:
    case Operation::Deploy:
    case Operation::Shutdown:
        return !payload.hasKeycode && !payload.hasModifiers && !payload.hasCandidateIndex &&
               !payload.hasSnapshotRevision && !payload.hasConvert;
    }
    return false;
}

bool parseCandidate(Parser& parser, Candidate& candidate) {
    if (!parser.objectStart()) return false;
    unsigned seen = 0;
    bool ended = false;
    if (!parser.objectEnd(ended)) return false;
    if (!ended) {
        while (true) {
            std::string key;
            if (!parser.string(key, kMaxFieldNameBytes) || !parser.colon()) return false;
            if (key == "text") {
                if ((seen & 1u) != 0 || !parser.string(candidate.text, kMaxTextBytes)) return false;
                seen |= 1u;
            } else if (key == "comment") {
                if ((seen & 2u) != 0 || !parser.string(candidate.comment, kMaxTextBytes)) return false;
                seen |= 2u;
            } else if (key == "label") {
                if ((seen & 4u) != 0 || !parser.string(candidate.label, kMaxTextBytes)) return false;
                seen |= 4u;
            } else {
                return false;
            }
            if (!parser.objectEnd(ended)) return false;
            if (ended) break;
            if (!parser.comma()) return false;
        }
    }
    return seen == 7u && textForWire(candidate.text) && textForWire(candidate.comment) && textForWire(candidate.label);
}

bool parseSnapshot(Parser& parser, Snapshot& snapshot) {
    if (!parser.objectStart()) return false;
    unsigned seen = 0;
    bool ended = false;
    if (!parser.objectEnd(ended)) return false;
    if (!ended) {
        while (true) {
            std::string key;
            if (!parser.string(key, kMaxFieldNameBytes) || !parser.colon()) return false;
            if (key == "commitText") {
                if ((seen & (1u << 0)) != 0 || !parser.string(snapshot.commitText, kMaxTextBytes)) return false;
                seen |= 1u << 0;
            } else if (key == "rawInput") {
                if ((seen & (1u << 1)) != 0 || !parser.string(snapshot.rawInput, kMaxTextBytes)) return false;
                seen |= 1u << 1;
            } else if (key == "preedit") {
                if ((seen & (1u << 2)) != 0 || !parser.string(snapshot.preedit, kMaxTextBytes)) return false;
                seen |= 1u << 2;
            } else if (key == "caretUtf8Offset") {
                std::uint64_t value = 0;
                if ((seen & (1u << 3)) != 0 || !parser.unsignedInteger(value) || value > kMaxTextBytes) return false;
                snapshot.caretUtf8Offset = static_cast<std::size_t>(value);
                seen |= 1u << 3;
            } else if (key == "candidates") {
                if ((seen & (1u << 4)) != 0 || !parser.arrayStart()) return false;
                snapshot.candidates.clear();
                bool arrayEnded = false;
                if (!parser.arrayEnd(arrayEnded)) return false;
                while (!arrayEnded) {
                    if (snapshot.candidates.size() >= kMaxCandidates) return false;
                    snapshot.candidates.emplace_back();
                    if (!parseCandidate(parser, snapshot.candidates.back())) return false;
                    if (!parser.arrayEnd(arrayEnded)) return false;
                    if (!arrayEnded && !parser.comma()) return false;
                }
                seen |= 1u << 4;
            } else if (key == "page") {
                std::uint64_t value = 0;
                if ((seen & (1u << 5)) != 0 || !parser.unsignedInteger(value) || value > std::numeric_limits<std::uint32_t>::max()) return false;
                snapshot.page = static_cast<std::uint32_t>(value);
                seen |= 1u << 5;
            } else if (key == "pageSize") {
                std::uint64_t value = 0;
                if ((seen & (1u << 6)) != 0 || !parser.unsignedInteger(value) || value > std::numeric_limits<std::uint32_t>::max()) return false;
                snapshot.pageSize = static_cast<std::uint32_t>(value);
                seen |= 1u << 6;
            } else if (key == "highlightedIndex") {
                std::uint64_t value = 0;
                if ((seen & (1u << 7)) != 0 || !parser.unsignedInteger(value) || value > std::numeric_limits<std::uint32_t>::max()) return false;
                snapshot.highlightedIndex = static_cast<std::uint32_t>(value);
                seen |= 1u << 7;
            } else if (key == "lastPage") {
                if ((seen & (1u << 8)) != 0 || !parser.boolean(snapshot.lastPage)) return false;
                seen |= 1u << 8;
            } else if (key == "asciiMode") {
                if ((seen & (1u << 9)) != 0 || !parser.boolean(snapshot.asciiMode)) return false;
                seen |= 1u << 9;
            } else if (key == "revision") {
                if ((seen & (1u << 10)) != 0 || !parser.unsignedInteger(snapshot.revision)) return false;
                seen |= 1u << 10;
            } else {
                return false;
            }
            if (!parser.objectEnd(ended)) return false;
            if (ended) break;
            if (!parser.comma()) return false;
        }
    }
    return seen == 0x7ffu && textForWire(snapshot.commitText) && textForWire(snapshot.rawInput) && textForWire(snapshot.preedit) &&
           caretBoundary(snapshot.preedit, snapshot.caretUtf8Offset);
}

bool validRequestForEncoding(const Request& request) noexcept {
    if (request.id.version != kProtocolVersion || !safeInteger(request.id.connectionEpoch) || !safeInteger(request.id.sessionId) ||
        !safeInteger(request.id.contextEpoch) || !safeInteger(request.id.sequence) || operationName(request.operation) == nullptr) return false;
    if (request.wantsAppearance && request.operation != Operation::CreateSession) return false;
    switch (request.operation) {
    case Operation::ProcessKey:
        return true;
    case Operation::SelectCandidate:
        return request.candidateIndex < kMaxCandidates && safeInteger(request.snapshotRevision);
    case Operation::Health:
    case Operation::CreateSession:
    case Operation::Finalize:
    case Operation::Cancel:
    case Operation::DestroySession:
    case Operation::Deploy:
    case Operation::Shutdown:
        return true;
    }
    return false;
}

bool validSnapshotForEncoding(const Snapshot& snapshot) noexcept {
    if (!textForWire(snapshot.commitText) || !textForWire(snapshot.rawInput) || !textForWire(snapshot.preedit) ||
        !caretBoundary(snapshot.preedit, snapshot.caretUtf8Offset) || snapshot.candidates.size() > kMaxCandidates ||
        !safeInteger(snapshot.revision)) return false;
    std::size_t estimatedBytes = 2048 + snapshot.candidates.size() * 64;
    const auto addString = [&](std::string_view value) {
        std::size_t escapedBytes = 2;
        for (const unsigned char byte : value) {
            if (byte < 0x20) {
                if (escapedBytes > kMaxFrameBytes - 6) return false;
                escapedBytes += 6;
            } else if (byte == static_cast<unsigned char>('"') || byte == static_cast<unsigned char>('\\')) {
                if (escapedBytes > kMaxFrameBytes - 2) return false;
                escapedBytes += 2;
            } else {
                if (escapedBytes == kMaxFrameBytes) return false;
                ++escapedBytes;
            }
        }
        if (estimatedBytes > kMaxFrameBytes - escapedBytes) return false;
        estimatedBytes += escapedBytes;
        return true;
    };
    if (!addString(snapshot.commitText) || !addString(snapshot.rawInput) || !addString(snapshot.preedit)) return false;
    for (const Candidate& candidate : snapshot.candidates) {
        if (!textForWire(candidate.text) || !textForWire(candidate.comment) || !textForWire(candidate.label) ||
            !addString(candidate.text) || !addString(candidate.comment) || !addString(candidate.label)) return false;
    }
    return true;
}

void appendIdentity(std::string& output, const Identity& id) {
    output += "\"version\":";
    appendNumber(output, id.version);
    output += ",\"connectionEpoch\":";
    appendNumber(output, id.connectionEpoch);
    output += ",\"sessionId\":";
    appendNumber(output, id.sessionId);
    output += ",\"contextEpoch\":";
    appendNumber(output, id.contextEpoch);
    output += ",\"sequence\":";
    appendNumber(output, id.sequence);
}

void appendSnapshot(std::string& output, const Snapshot& snapshot) {
    output += "\"snapshot\":{";
    output += "\"commitText\":";
    appendJsonString(output, snapshot.commitText);
    output += ",\"rawInput\":";
    appendJsonString(output, snapshot.rawInput);
    output += ",\"preedit\":";
    appendJsonString(output, snapshot.preedit);
    output += ",\"caretUtf8Offset\":";
    appendNumber(output, snapshot.caretUtf8Offset);
    output += ",\"candidates\":[";
    for (std::size_t i = 0; i < snapshot.candidates.size(); ++i) {
        if (i != 0) output.push_back(',');
        const Candidate& candidate = snapshot.candidates[i];
        output += "{";
        output += "\"text\":";
        appendJsonString(output, candidate.text);
        output += ",\"comment\":";
        appendJsonString(output, candidate.comment);
        output += ",\"label\":";
        appendJsonString(output, candidate.label);
        output += "}";
    }
    output += "],\"page\":";
    appendNumber(output, snapshot.page);
    output += ",\"pageSize\":";
    appendNumber(output, snapshot.pageSize);
    output += ",\"highlightedIndex\":";
    appendNumber(output, snapshot.highlightedIndex);
    output += ",\"lastPage\":";
    output += snapshot.lastPage ? "true" : "false";
    output += ",\"asciiMode\":";
    output += snapshot.asciiMode ? "true" : "false";
    output += ",\"revision\":";
    appendNumber(output, snapshot.revision);
    output += "}";
}
} // namespace

bool Identity::operator==(const Identity& other) const {
    return version == other.version && connectionEpoch == other.connectionEpoch && sessionId == other.sessionId &&
           contextEpoch == other.contextEpoch && sequence == other.sequence;
}

bool validUtf8(std::string_view text) noexcept {
    for (std::size_t i = 0; i < text.size();) {
        const unsigned char first = static_cast<unsigned char>(text[i]);
        std::size_t length = 0;
        std::uint32_t code = 0;
        if (first <= 0x7f) {
            length = 1;
            code = first;
        } else if (first >= 0xc2 && first <= 0xdf) {
            length = 2;
            code = first & 0x1f;
        } else if (first >= 0xe0 && first <= 0xef) {
            length = 3;
            code = first & 0x0f;
        } else if (first >= 0xf0 && first <= 0xf4) {
            length = 4;
            code = first & 0x07;
        } else {
            return false;
        }
        if (i + length > text.size()) return false;
        for (std::size_t j = 1; j < length; ++j) {
            const unsigned char continuation = static_cast<unsigned char>(text[i + j]);
            if ((continuation & 0xc0) != 0x80) return false;
            code = (code << 6) | (continuation & 0x3f);
        }
        if ((length == 2 && code < 0x80) || (length == 3 && code < 0x800) || (length == 4 && code < 0x10000) ||
            code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) return false;
        i += length;
    }
    return true;
}

std::u16string utf8ToUtf16(std::string_view text) {
    std::u16string output;
    if (!validUtf8(text)) return output;
    output.reserve(text.size());
    for (std::size_t i = 0; i < text.size();) {
        const unsigned char first = static_cast<unsigned char>(text[i]);
        std::uint32_t code = 0;
        std::size_t length = 1;
        if (first <= 0x7f) code = first;
        else if (first <= 0xdf) { code = first & 0x1f; length = 2; }
        else if (first <= 0xef) { code = first & 0x0f; length = 3; }
        else { code = first & 0x07; length = 4; }
        for (std::size_t j = 1; j < length; ++j) code = (code << 6) | (static_cast<unsigned char>(text[i + j]) & 0x3f);
        if (code <= 0xffff) output.push_back(static_cast<char16_t>(code));
        else {
            code -= 0x10000;
            output.push_back(static_cast<char16_t>(0xd800 | (code >> 10)));
            output.push_back(static_cast<char16_t>(0xdc00 | (code & 0x3ff)));
        }
        i += length;
    }
    return output;
}

std::size_t utf16Caret(std::string_view text, std::size_t byteOffset) {
    if (!caretBoundary(text, byteOffset)) return 0;
    std::size_t units = 0;
    for (std::size_t i = 0; i < byteOffset;) {
        const unsigned char first = static_cast<unsigned char>(text[i]);
        const std::size_t length = first <= 0x7f ? 1 : first <= 0xdf ? 2 : first <= 0xef ? 3 : 4;
        units += length == 4 ? 2 : 1;
        i += length;
    }
    return units;
}

std::string encodeRequest(const Request& request) {
    if (!validRequestForEncoding(request)) return {};
    std::string output;
    output.reserve(256);
    output.push_back('{');
    appendIdentity(output, request.id);
    output += ",\"operation\":";
    appendJsonString(output, operationName(request.operation));
    output += ",\"payload\":{";
    bool first = true;
    auto comma = [&] {
        if (!first) output.push_back(',');
        first = false;
    };
    switch (request.operation) {
    case Operation::ProcessKey:
        comma(); output += "\"keycode\":"; appendNumber(output, request.keycode);
        comma(); output += "\"modifiers\":"; appendNumber(output, request.modifiers);
        break;
    case Operation::SelectCandidate:
        comma(); output += "\"candidateIndex\":"; appendNumber(output, request.candidateIndex);
        comma(); output += "\"snapshotRevision\":"; appendNumber(output, request.snapshotRevision);
        break;
    case Operation::Finalize:
        comma(); output += "\"convert\":"; output += request.convert ? "true" : "false";
        break;
    case Operation::CreateSession:
        if (request.wantsAppearance) { comma(); output += "\"wantsAppearance\":true"; }
        break;
    case Operation::Health:
    case Operation::Cancel:
    case Operation::DestroySession:
    case Operation::Deploy:
    case Operation::Shutdown:
        break;
    }
    output += "}}";
    if (output.size() > kMaxFrameBytes) return {};
    return output;
}

std::string encodeResponse(const Response& response) {
    if (response.id.version != kProtocolVersion || !safeInteger(response.id.connectionEpoch) || !safeInteger(response.id.sessionId) ||
        !safeInteger(response.id.contextEpoch) || !safeInteger(response.id.sequence) || statusName(response.status) == nullptr ||
        (response.status != Status::Ok && response.handled) || !validSnapshotForEncoding(response.snapshot)) return {};
    if (response.appearance && (!response.handled || response.id.sessionId == 0 ||
        response.appearance->json.empty() || response.appearance->json.size() > kMaxCandidateAppearanceBytes ||
        !textForWire(response.appearance->json))) return {};
    std::string output;
    output.reserve(512);
    output.push_back('{');
    appendIdentity(output, response.id);
    output += ",\"status\":";
    appendJsonString(output, statusName(response.status));
    output += ",\"handled\":";
    output += response.handled ? "true" : "false";
    if (response.diagnosticCode != 0) {
        output += ",\"diagnosticCode\":";
        appendNumber(output, response.diagnosticCode);
    }
    output.push_back(',');
    appendSnapshot(output, response.snapshot);
    if (response.appearance) {
        output += ",\"candidateStyle\":";
        appendJsonString(output, response.appearance->json);
        output += ",\"candidateDark\":";
        output += response.appearance->dark ? "true" : "false";
    }
    output.push_back('}');
    if (output.size() > kMaxFrameBytes) return {};
    return output;
}

std::optional<Request> decodeRequest(std::string_view json) noexcept {
    try {
        if (json.empty() || json.size() > kMaxFrameBytes || !validUtf8(json)) return std::nullopt;
        Parser parser(json);
        if (!parser.objectStart()) return std::nullopt;
        Request request;
        RequestPayload payload;
        unsigned seen = 0;
        std::string operation;
        bool ended = false;
        if (!parser.objectEnd(ended)) return std::nullopt;
        if (!ended) {
            while (true) {
                std::string key;
                if (!parser.string(key, kMaxFieldNameBytes) || !parser.colon()) return std::nullopt;
                if (key == "version" || key == "connectionEpoch" || key == "sessionId" || key == "contextEpoch" || key == "sequence") {
                    if (!parseIdentityField(parser, key, request.id, seen)) return std::nullopt;
                } else if (key == "operation") {
                    if ((seen & (1u << 5)) != 0 || !parser.string(operation, 32)) return std::nullopt;
                    seen |= 1u << 5;
                } else if (key == "payload") {
                    if ((seen & (1u << 6)) != 0 || !parseRequestPayload(parser, payload)) return std::nullopt;
                    seen |= 1u << 6;
                } else {
                    return std::nullopt;
                }
                if (!parser.objectEnd(ended)) return std::nullopt;
                if (ended) break;
                if (!parser.comma()) return std::nullopt;
            }
        }
        if (!parser.end() || seen != 0x7fu) return std::nullopt;
        const auto parsedOperation = parseOperation(operation);
        if (!parsedOperation || !validateRequestPayload(*parsedOperation, payload)) return std::nullopt;
        request.operation = *parsedOperation;
        request.keycode = payload.keycode;
        request.modifiers = payload.modifiers;
        request.candidateIndex = payload.candidateIndex;
        request.snapshotRevision = payload.snapshotRevision;
        request.convert = payload.convert;
        request.wantsAppearance = payload.wantsAppearance;
        return request;
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<Response> decodeResponse(std::string_view json) noexcept {
    try {
        if (json.empty() || json.size() > kMaxFrameBytes || !validUtf8(json)) return std::nullopt;
        Parser parser(json);
        if (!parser.objectStart()) return std::nullopt;
        Response response;
        Response::CandidateAppearance appearance;
        unsigned seen = 0;
        std::string status;
        bool ended = false;
        if (!parser.objectEnd(ended)) return std::nullopt;
        if (!ended) {
            while (true) {
                std::string key;
                if (!parser.string(key, kMaxFieldNameBytes) || !parser.colon()) return std::nullopt;
                if (key == "version" || key == "connectionEpoch" || key == "sessionId" || key == "contextEpoch" || key == "sequence") {
                    if (!parseIdentityField(parser, key, response.id, seen)) return std::nullopt;
                } else if (key == "status") {
                    if ((seen & (1u << 5)) != 0 || !parser.string(status, 32)) return std::nullopt;
                    seen |= 1u << 5;
            } else if (key == "handled") {
                if ((seen & (1u << 6)) != 0 || !parser.boolean(response.handled)) return std::nullopt;
                seen |= 1u << 6;
            } else if (key == "diagnosticCode") {
                std::uint64_t value = 0;
                if ((seen & (1u << 7)) != 0 || !parser.unsignedInteger(value) ||
                    value > std::numeric_limits<std::uint32_t>::max()) return std::nullopt;
                response.diagnosticCode = static_cast<std::uint32_t>(value);
                seen |= 1u << 7;
            } else if (key == "snapshot") {
                if ((seen & (1u << 8)) != 0 || !parseSnapshot(parser, response.snapshot)) return std::nullopt;
                seen |= 1u << 8;
            } else if (key == "candidateStyle") {
                if ((seen & (1u << 9)) != 0 || !parser.string(appearance.json, kMaxCandidateAppearanceBytes) ||
                    appearance.json.empty() || !textForWire(appearance.json)) return std::nullopt;
                seen |= 1u << 9;
            } else if (key == "candidateDark") {
                if ((seen & (1u << 10)) != 0 || !parser.boolean(appearance.dark)) return std::nullopt;
                seen |= 1u << 10;
            } else {
                    return std::nullopt;
                }
                if (!parser.objectEnd(ended)) return std::nullopt;
                if (ended) break;
                if (!parser.comma()) return std::nullopt;
            }
        }
        if (!parser.end() || (seen & 0x17fu) != 0x17fu) return std::nullopt;
        const auto parsedStatus = parseStatus(status);
        if (!parsedStatus || response.id.version != kProtocolVersion || (*parsedStatus != Status::Ok && response.handled) ||
            !validSnapshotForEncoding(response.snapshot)) return std::nullopt;
        constexpr unsigned appearanceFields = (1u << 9) | (1u << 10);
        if ((seen & appearanceFields) != 0) {
            if ((seen & appearanceFields) != appearanceFields || !response.handled || response.id.sessionId == 0) return std::nullopt;
            response.appearance = std::move(appearance);
        }
        response.status = *parsedStatus;
        return response;
    } catch (...) {
        return std::nullopt;
    }
}

std::vector<std::uint8_t> frame(std::string_view utf8) {
    if (utf8.empty() || utf8.size() > kMaxFrameBytes || !validUtf8(utf8)) return {};
    std::vector<std::uint8_t> result(4 + utf8.size());
    const auto length = static_cast<std::uint32_t>(utf8.size());
    result[0] = static_cast<std::uint8_t>(length & 0xff);
    result[1] = static_cast<std::uint8_t>((length >> 8) & 0xff);
    result[2] = static_cast<std::uint8_t>((length >> 16) & 0xff);
    result[3] = static_cast<std::uint8_t>((length >> 24) & 0xff);
    std::copy(utf8.begin(), utf8.end(), result.begin() + 4);
    return result;
}

std::optional<std::uint32_t> frameLength(const std::uint8_t* header, std::size_t size) noexcept {
    if (header == nullptr || size < 4) return std::nullopt;
    const std::uint32_t length = static_cast<std::uint32_t>(header[0]) |
                                 (static_cast<std::uint32_t>(header[1]) << 8) |
                                 (static_cast<std::uint32_t>(header[2]) << 16) |
                                 (static_cast<std::uint32_t>(header[3]) << 24);
    if (length == 0 || length > kMaxFrameBytes) return std::nullopt;
    return length;
}

const char* statusName(Status status) noexcept {
    switch (status) {
    case Status::Ok: return "ok";
    case Status::Unavailable: return "unavailable";
    case Status::Busy: return "busy";
    case Status::Stale: return "stale";
    case Status::UnsupportedVersion: return "unsupportedVersion";
    case Status::InvalidRequest: return "invalidRequest";
    case Status::EngineFailure: return "engineFailure";
    }
    return nullptr;
}
} // namespace xmjd
