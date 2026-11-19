#include "../Native/Protocol/Broker.h"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#define CHECK(condition)                                                                                                      \
    do {                                                                                                                     \
        if (!(condition)) {                                                                                                  \
            std::cerr << "CHECK failed: " #condition << " at " << __FILE__ << ':' << __LINE__ << '\n';                    \
            return 1;                                                                                                      \
        }                                                                                                                  \
    } while (false)

namespace {
using namespace xmjd;

class FakeEngine final : public Engine {
public:
    struct State {
        std::string raw;
        std::string commit;
        bool selected = false;
    };

    bool ready() const override {
        ++readyCalls;
        if (throwOnReady) throw std::runtime_error("ready");
        return available;
    }

    std::uint64_t createSession() override {
        ++createAttempts;
        if (throwOnCreate) throw std::runtime_error("create");
        if (failCreate) return 0;
        const std::uint64_t id = nextId++;
        states.emplace(id, State{});
        ++created;
        return id;
    }

    void destroySession(std::uint64_t session) override {
        ++destroyAttempts;
        if (throwOnDestroy) throw std::runtime_error("destroy");
        states.erase(session);
        ++destroyed;
    }

    bool processKey(std::uint64_t session, std::uint32_t key, std::uint32_t) override {
        ++processed;
        if (throwOnProcess) throw std::runtime_error("process");
        auto it = states.find(session);
        if (it == states.end() || !handleProcess) return false;
        it->second.raw.push_back(static_cast<char>(key));
        return true;
    }

    bool selectCandidate(std::uint64_t session, std::uint32_t pageIndex) override {
        ++selected;
        auto it = states.find(session);
        if (it == states.end() || pageIndex >= 2) return false;
        it->second.commit = pageIndex == 0 ? "一" : "二";
        it->second.selected = true;
        return true;
    }

    void finalize(std::uint64_t session, bool convert) override {
        ++finalized;
        auto it = states.find(session);
        if (it != states.end() && convert) {
            it->second.commit = it->second.raw;
            it->second.raw.clear();
        }
    }

    void cancel(std::uint64_t session) override {
        ++cancelled;
        auto it = states.find(session);
        if (it != states.end()) it->second.raw.clear();
    }

    Snapshot snapshot(std::uint64_t session, bool drainCommit = true) override {
        ++snapshotCalls;
        if (throwOnSnapshot) throw std::runtime_error("snapshot");
        auto it = states.find(session);
        Snapshot result;
        if (it == states.end()) return result;
        if (drainCommit) {
            result.commitText = std::move(it->second.commit);
            it->second.commit.clear();
        }
        result.rawInput = it->second.raw;
        result.preedit = it->second.raw;
        result.caretUtf8Offset = result.preedit.size();
        result.pageSize = 2;
        result.candidates = {{"一", "", "1"}, {"二", "", "2"}};
        result.asciiMode = asciiMode;
        return result;
    }

    bool available = true;
    bool throwOnReady = false;
    bool throwOnCreate = false;
    bool failCreate = false;
    bool throwOnProcess = false;
    bool throwOnDestroy = false;
    bool throwOnSnapshot = false;
    bool handleProcess = true;
    bool asciiMode = false;
    mutable std::size_t readyCalls = 0;
    std::uint64_t nextId = 1;
    std::size_t createAttempts = 0;
    std::size_t destroyAttempts = 0;
    std::size_t created = 0;
    std::size_t destroyed = 0;
    std::size_t processed = 0;
    std::size_t selected = 0;
    std::size_t finalized = 0;
    std::size_t cancelled = 0;
    std::size_t snapshotCalls = 0;
    std::unordered_map<std::uint64_t, State> states;
};

Request request(Operation operation, std::uint64_t sequence, std::uint64_t session = 0, std::uint64_t context = 0) {
    Request result;
    result.id.connectionEpoch = 77;
    result.id.sessionId = session;
    result.id.contextEpoch = context;
    result.id.sequence = sequence;
    result.operation = operation;
    return result;
}

int testUnicodeAndCodec() {
    CHECK(validUtf8("中文😀"));
    CHECK(!validUtf8(std::string("\xC0\x80", 2)));
    CHECK(!validUtf8(std::string("\xED\xA0\x80", 3)));
    CHECK(utf8ToUtf16("A😀").size() == 3);
    CHECK(utf16Caret("A😀中", 0) == 0);
    CHECK(utf16Caret("A😀中", 1) == 1);
    CHECK(utf16Caret("A😀中", 5) == 3);
    CHECK(utf16Caret("A😀中", 2) == 0);

    Request input = request(Operation::ProcessKey, 1);
    input.keycode = 97;
    input.modifiers = 4;
    const std::string encoded = encodeRequest(input);
    CHECK(!encoded.empty());
    const auto decoded = decodeRequest(encoded);
    CHECK(decoded.has_value());
    CHECK(decoded->id == input.id);
    CHECK(decoded->operation == Operation::ProcessKey);
    CHECK(decoded->keycode == 97);
    CHECK(decoded->modifiers == 4);
    CHECK(!decodeRequest("{\"version\":1,\"version\":1,\"connectionEpoch\":77,\"sessionId\":0,\"contextEpoch\":0,\"sequence\":1,\"operation\":\"health\",\"payload\":{}}"));
    CHECK(!decodeRequest("{\"version\":1,\"connectionEpoch\":77,\"sessionId\":0,\"contextEpoch\":0,\"sequence\":1,\"operation\":\"health\",\"payload\":[],\"extra\":1}"));
    CHECK(!decodeRequest("{\"version\":1,\"connectionEpoch\":9007199254740992,\"sessionId\":0,\"contextEpoch\":0,\"sequence\":1,\"operation\":\"health\",\"payload\":{}}"));
    CHECK(!decodeRequest("{\"version\":1,\"connectionEpoch\":77,\"sessionId\":0,\"contextEpoch\":0,\"sequence\":1.0,\"operation\":\"health\",\"payload\":{}}"));
    CHECK(!decodeRequest("{\"version\":1,\"connectionEpoch\":77,\"sessionId\":0,\"contextEpoch\":0,\"sequence\":1,\"operation\":\"health\",\"payload\":{},}"));
    CHECK(!decodeRequest("{\"version\":1,\"connectionEpoch\":77,\"sessionId\":0,\"contextEpoch\":0,\"sequence\":1,\"operation\":\"health\",\"payload\":{\"unused\":[],}}"));
    const Request shutdown = request(Operation::Shutdown, 1);
    const auto decodedShutdown = decodeRequest(encodeRequest(shutdown));
    CHECK(decodedShutdown && decodedShutdown->operation == Operation::Shutdown);
    CHECK(decodedShutdown->id == shutdown.id);
    CHECK(!decodeRequest("{\"version\":1,\"connectionEpoch\":77,\"sessionId\":0,\"contextEpoch\":0,\"sequence\":1,\"operation\":\"shutdown\",\"payload\":{\"keycode\":1,\"modifiers\":0}}"));
    const auto bytes = frame(encoded);
    CHECK(bytes.size() == encoded.size() + 4);
    CHECK(bytes[0] == static_cast<std::uint8_t>(encoded.size() & 0xff));
    CHECK(frameLength(bytes.data(), 4).value() == encoded.size());
    const std::uint8_t oversized[] = {0xff, 0xff, 0x10, 0x00};
    CHECK(!frameLength(oversized, sizeof(oversized)));

    Response response;
    response.id = input.id;
    response.snapshot.preedit = "A😀中";
    response.snapshot.caretUtf8Offset = 5;
    response.snapshot.candidates = {{"一", "注", "1"}};
    response.snapshot.revision = 9;
    const auto responseText = encodeResponse(response);
    CHECK(!responseText.empty());
    const auto decodedResponse = decodeResponse(responseText);
    CHECK(decodedResponse.has_value());
    CHECK(decodedResponse->snapshot.caretUtf8Offset == 5);
    CHECK(decodedResponse->snapshot.candidates.size() == 1);
    CHECK(decodedResponse->diagnosticCode == 0);

    Response deployFailure;
    deployFailure.id = request(Operation::Deploy, 1).id;
    deployFailure.status = Status::EngineFailure;
    deployFailure.diagnosticCode = 16;
    const std::string deployFailureText = encodeResponse(deployFailure);
    CHECK(!deployFailureText.empty());
    const auto decodedDeployFailure = decodeResponse(deployFailureText);
    CHECK(decodedDeployFailure && decodedDeployFailure->diagnosticCode == 16);
    CHECK(decodedDeployFailure->snapshot.page == 0);
    std::string trailingCommaResponse = responseText;
    trailingCommaResponse.insert(trailingCommaResponse.size() - 1, ",");
    CHECK(!decodeResponse(trailingCommaResponse));
    std::string trailingCommaCandidates = responseText;
    const std::size_t candidateArrayEnd = trailingCommaCandidates.find("],\"page\"");
    CHECK(candidateArrayEnd != std::string::npos);
    trailingCommaCandidates.insert(candidateArrayEnd, ",");
    CHECK(!decodeResponse(trailingCommaCandidates));
    Response invalidResponse = response;
    invalidResponse.status = Status::EngineFailure;
    invalidResponse.handled = true;
    CHECK(encodeResponse(invalidResponse).empty());
    std::string invalidResponseText = responseText;
    const std::size_t statusOffset = invalidResponseText.find("\"status\":\"ok\"");
    const std::size_t handledOffset = invalidResponseText.find("\"handled\":false");
    CHECK(statusOffset != std::string::npos);
    CHECK(handledOffset != std::string::npos);
    invalidResponseText.replace(handledOffset, std::string("\"handled\":false").size(), "\"handled\":true");
    invalidResponseText.replace(statusOffset, std::string("\"status\":\"ok\"").size(), "\"status\":\"engineFailure\"");
    CHECK(!decodeResponse(invalidResponseText));
    return 0;
}

int testCandidateAppearance() {
    Request create = request(Operation::CreateSession, 1, 0, 9);
    const auto legacy = decodeRequest(encodeRequest(create));
    CHECK(legacy && !legacy->wantsAppearance);
    create.wantsAppearance = true;
    const auto preferred = decodeRequest(encodeRequest(create));
    CHECK(preferred && preferred->wantsAppearance && preferred->operation == Operation::CreateSession);
    Request notCreate = request(Operation::Health, 1);
    notCreate.wantsAppearance = true;
    CHECK(encodeRequest(notCreate).empty());
    CHECK(!decodeRequest(R"({"version":1,"connectionEpoch":77,"sessionId":0,"contextEpoch":0,"sequence":1,"operation":"health","payload":{"wantsAppearance":false}})"));

    Response response;
    response.id = create.id;
    response.id.sessionId = 1;
    response.handled = true;
    const auto classic = encodeResponse(response);
    CHECK(!classic.empty() && classic.find("candidateStyle") == std::string::npos);
    response.appearance = Response::CandidateAppearance{R"({"name":"节庆流光","id":"fiesta"})", true};
    const auto encoded = encodeResponse(response);
    const auto decoded = decodeResponse(encoded);
    CHECK(decoded && decoded->appearance && decoded->appearance->dark &&
          decoded->appearance->json == response.appearance->json);
    CHECK(!decodeResponse(classic.substr(0, classic.size() - 1) + R"(,"candidateDark":true})"));
    CHECK(!decodeResponse(classic.substr(0, classic.size() - 1) + R"(,"candidateStyle":"{}"})"));
    response.appearance->json.assign(kMaxCandidateAppearanceBytes + 1, 'a');
    CHECK(encodeResponse(response).empty());
    return 0;
}

int testDecoderLimits() {
    const std::string prefix = "{\"version\":1,\"connectionEpoch\":77,\"sessionId\":1,\"contextEpoch\":9,\"sequence\":";
    const std::string suffix = ",\"operation\":\"health\",\"payload\":{}}";
    for (const std::string number : {"-1", "+1", "01", "0.0", "0e1", "1.0", "1e0", "1E+1", "null", "true",
                                     "\"1\"", "9007199254740992", "18446744073709551616"}) {
        CHECK(!decodeRequest(prefix + number + suffix));
    }
    CHECK(decodeRequest(prefix + "9007199254740991" + suffix));

    const std::string candidatePrefix = prefix + "1,\"operation\":\"selectCandidate\",\"payload\":{\"candidateIndex\":";
    CHECK(decodeRequest(candidatePrefix + std::to_string(kMaxCandidates - 1) + ",\"snapshotRevision\":0}}"));
    CHECK(!decodeRequest(candidatePrefix + std::to_string(kMaxCandidates) + ",\"snapshotRevision\":0}}"));
    Request candidate = request(Operation::SelectCandidate, 1, 1, 9);
    candidate.candidateIndex = static_cast<std::uint32_t>(kMaxCandidates);
    CHECK(encodeRequest(candidate).empty());

    const std::string keyPrefix = prefix + "1,\"operation\":\"processKey\",\"payload\":{\"keycode\":";
    CHECK(decodeRequest(keyPrefix + "4294967295,\"modifiers\":4294967295}}"));
    CHECK(!decodeRequest(keyPrefix + "4294967296,\"modifiers\":0}}"));
    CHECK(!decodeRequest(keyPrefix + "1,\"modifiers\":4294967296}}"));
    CHECK(!decodeRequest(keyPrefix + "1,\"keycode\":1,\"modifiers\":0}}"));
    CHECK(!decodeRequest(prefix + "1,\"operation\":\"health\",\"payload\":{},\"\\u0073equence\":2}"));
    CHECK(!decodeRequest(prefix + "1,\"operation\":\"health\",\"payload\":{\"unused\":{\"nested\":[]}}}"));
    CHECK(!decodeRequest(prefix + "1,\"operation\":\"finalize\",\"payload\":{\"convert\":truefalse}}"));

    const std::string deploy = encodeRequest(request(Operation::Deploy, 1));
    CHECK(!deploy.empty());
    const auto decodedDeploy = decodeRequest(deploy);
    CHECK(decodedDeploy && decodedDeploy->operation == Operation::Deploy);

    const std::string health = encodeRequest(request(Operation::Health, 1));
    for (std::size_t length = 0; length < health.size(); ++length) {
        CHECK(!decodeRequest(std::string_view(health).substr(0, length)));
    }
    CHECK(!decodeRequest(health + "{}"));
    CHECK(!decodeRequest(health + std::string(1, '\0')));
    CHECK(!decodeRequest(std::string(kMaxFrameBytes + 1, ' ')));

    Response response;
    response.id = request(Operation::ProcessKey, 1, 1, 9).id;
    response.snapshot.preedit = "A😀中";
    response.snapshot.caretUtf8Offset = 5;
    response.snapshot.candidates = {{"一", "注", "1"}};
    const std::string encoded = encodeResponse(response);
    const std::vector<std::pair<std::string, std::string>> malformed = {
        {"\"version\":1", "\"version\":2"},
        {"\"status\":\"ok\"", "\"status\":\"unknown\""},
        {"\"handled\":false", "\"handled\":false,\"handled\":false"},
        {"\"handled\":false", "\"handled\":0"},
        {"\"handled\":false", "\"handled\":false,\"diagnosticCode\":4294967296"},
        {"\"revision\":0", "\"revision\":0.0"},
        {"\"revision\":0", "\"revision\":9007199254740992"},
        {"\"caretUtf8Offset\":5", "\"caretUtf8Offset\":2"},
        {"\"caretUtf8Offset\":5", "\"caretUtf8Offset\":100"},
        {"\"commitText\":\"\"", "\"commitText\":\"\\uD800\""},
        {"\"commitText\":\"\"", "\"commitText\":\"\\uDC00\""},
        {"\"commitText\":\"\"", "\"commitText\":\"\\uD800\\u0041\""},
        {"\"commitText\":\"\"", "\"commitText\":\"\\uD800\\uDC0\""},
        {"\"commitText\":\"\"", "\"commitText\":\"\\u12x4\""},
        {"\"commitText\":\"\"", "\"commitText\":\"\\x41\""},
        {"\"commitText\":\"\"", "\"commitText\":\"\n\""},
        {"\"commitText\":\"\"", "\"commitText\":null"},
        {"\"text\":\"一\"", "\"text\":\"一\",\"\\u0074ext\":\"二\""},
        {"\"comment\":\"注\",", ""},
        {"\"label\":\"1\"}", "\"label\":\"1\",}"},
        {"\"label\":\"1\"}]", "\"label\":\"1\"},]"},
        {"\"candidates\":[", "\"candidates\":[] ,\"extra\":["},
    };
    for (const auto& change : malformed) {
        std::string invalid = encoded;
        const auto position = invalid.find(change.first);
        CHECK(position != std::string::npos);
        invalid.replace(position, change.first.size(), change.second);
        CHECK(!decodeResponse(invalid));
    }
    for (std::size_t length = 0; length < encoded.size(); ++length) {
        CHECK(!decodeResponse(std::string_view(encoded).substr(0, length)));
    }
    for (const std::string& invalidUtf8 : {std::string("\x80", 1), std::string("\xC0\x80", 2), std::string("\xE0\x80\x80", 3),
                                             std::string("\xED\xA0\x80", 3), std::string("\xF0\x80\x80\x80", 4),
                                             std::string("\xF4\x90\x80\x80", 4), std::string("\xF0\x9F\x98", 3)}) {
        CHECK(!validUtf8(invalidUtf8));
        CHECK(!decodeRequest(health + invalidUtf8));
        CHECK(!decodeResponse(encoded + invalidUtf8));
    }

    Response empty;
    std::string escaped = encodeResponse(empty);
    const auto commitPosition = escaped.find("\"commitText\":\"\"");
    CHECK(commitPosition != std::string::npos);
    escaped.replace(commitPosition, std::string("\"commitText\":\"\"").size(),
                    "\"commitText\":\"\\u0000\\uD83D\\uDE00\"");
    const auto decodedEscapes = decodeResponse(escaped);
    CHECK(decodedEscapes);
    CHECK(decodedEscapes->snapshot.commitText == std::string("\0", 1) + "😀");

    empty.snapshot.commitText.assign(kMaxTextBytes, 'a');
    CHECK(decodeResponse(encodeResponse(empty)));
    std::string oversizedText = encodeResponse(empty);
    oversizedText.insert(oversizedText.find("\"commitText\":\"") + std::string("\"commitText\":\"").size(), "a");
    CHECK(!decodeResponse(oversizedText));
    empty.snapshot.commitText.clear();
    empty.snapshot.candidates.assign(kMaxCandidates, Candidate{});
    const std::string fullCandidates = encodeResponse(empty);
    CHECK(decodeResponse(fullCandidates));
    std::string oversizedCandidates = fullCandidates;
    oversizedCandidates.insert(oversizedCandidates.find("\"candidates\":[") + std::string("\"candidates\":[").size(),
                               "{\"text\":\"\",\"comment\":\"\",\"label\":\"\"},");
    CHECK(!decodeResponse(oversizedCandidates));
    CHECK(!decodeResponse(std::string(kMaxFrameBytes + 1, ' ')));

    const std::uint8_t zero[] = {0, 0, 0, 0};
    const std::uint8_t maximum[] = {0, 0, 0x10, 0};
    CHECK(!frameLength(nullptr, 4));
    CHECK(!frameLength(maximum, 3));
    CHECK(!frameLength(zero, 4));
    CHECK(frameLength(maximum, 4).value() == kMaxFrameBytes);
    CHECK(frame("").empty());
    CHECK(frame(std::string(kMaxFrameBytes + 1, 'a')).empty());
    return 0;
}

int testUnhandledSnapshotFailure() {
    for (const Operation operation : {Operation::ProcessKey, Operation::SelectCandidate}) {
        FakeEngine engine;
        BrokerConnection broker(engine);
        const Response first = broker.dispatch(request(Operation::CreateSession, 1, 0, 9));
        const Response second = broker.dispatch(request(Operation::CreateSession, 2, 0, 10));
        CHECK(first.status == Status::Ok);
        CHECK(second.status == Status::Ok);
        engine.handleProcess = false;
        engine.throwOnSnapshot = true;
        Request unhandled = request(operation, 3, first.id.sessionId, 9);
        unhandled.keycode = 'x';
        unhandled.candidateIndex = 2;
        const Response failed = broker.dispatch(unhandled);
        CHECK(failed.status == Status::EngineFailure);
        CHECK(!failed.handled);
        CHECK(failed.snapshot.commitText.empty());
        CHECK(failed.snapshot.rawInput.empty());
        CHECK(failed.snapshot.preedit.empty());
        CHECK(engine.snapshotCalls == 3);
        CHECK(engine.states.empty());
        CHECK(engine.destroyed == 2);
        CHECK(broker.dispatch(request(Operation::Health, 4)).status == Status::Unavailable);
        CHECK(broker.dispatch(request(Operation::CreateSession, 5, 0, 11)).status == Status::Unavailable);
        CHECK(engine.created == 2);
        CHECK(engine.snapshotCalls == 3);
    }
    return 0;
}

int testModifierSideEffects() {
    // ascii_composer may emit commit_code on Shift release and still return
    // kNoop. It must reach TSF once while native Shift itself passes through.
    FakeEngine engine;
    engine.handleProcess = false;
    engine.asciiMode = true;
    BrokerConnection broker(engine);
    const Response created = broker.dispatch(request(Operation::CreateSession, 1, 0, 9));
    CHECK(created.status == Status::Ok);
    CHECK(created.snapshot.asciiMode);
    CHECK(created.snapshot.candidates.empty());
    engine.states.begin()->second.commit = "ni";
    Request release = request(Operation::ProcessKey, 2, created.id.sessionId, 9);
    release.keycode = 0xffe1;
    release.modifiers = kReleaseMask;
    const Response switched = broker.dispatch(release);
    CHECK(switched.status == Status::Ok);
    CHECK(!switched.handled);
    CHECK(switched.snapshot.commitText == "ni");
    CHECK(switched.snapshot.revision == 1);
    CHECK(engine.states.begin()->second.commit.empty());
    CHECK(broker.dispatch(release).status == Status::Stale);
    release.id.sequence = 3;
    CHECK(broker.dispatch(release).snapshot.commitText.empty());
    CHECK(engine.snapshotCalls == 3);
    CHECK(decodeResponse(encodeResponse(switched))->snapshot.commitText == "ni");
    return 0;
}

int testCreateFailureIdentity() {
    FakeEngine engine;
    BrokerConnection broker(engine);
    Request create = request(Operation::CreateSession, 1, 0, 9);
    engine.failCreate = true;
    Response failed = broker.dispatch(create);
    CHECK(failed.status == Status::EngineFailure);
    CHECK(!failed.handled);
    CHECK(failed.id == create.id);
    CHECK(decodeResponse(encodeResponse(failed)).value().id == create.id);
    CHECK(engine.states.empty());

    engine.failCreate = false;
    create.id.sequence = 2;
    const Response created = broker.dispatch(create);
    CHECK(created.status == Status::Ok);
    CHECK(created.handled);
    CHECK(created.id.sessionId == 1);
    CHECK(created.id.connectionEpoch == create.id.connectionEpoch);
    CHECK(created.id.contextEpoch == create.id.contextEpoch);
    CHECK(created.id.sequence == create.id.sequence);
    CHECK(decodeResponse(encodeResponse(created)).value().id == created.id);

    create.id.sequence = 3;
    create.id.contextEpoch = 0;
    failed = broker.dispatch(create);
    CHECK(failed.status == Status::InvalidRequest);
    CHECK(failed.id == create.id);
    CHECK(!failed.handled);
    create.id.contextEpoch = 9;
    engine.available = false;
    create.id.sequence = 4;
    failed = broker.dispatch(create);
    CHECK(failed.status == Status::Unavailable);
    CHECK(failed.id == create.id);
    CHECK(!failed.handled);
    CHECK(decodeResponse(encodeResponse(failed)).value().id == create.id);
    engine.available = true;

    for (std::uint64_t sequence = 5; sequence <= 35; ++sequence) {
        create.id.sequence = sequence;
        CHECK(broker.dispatch(create).status == Status::Ok);
    }
    create.id.sequence = 36;
    failed = broker.dispatch(create);
    CHECK(failed.status == Status::Busy);
    CHECK(failed.id == create.id);
    CHECK(!failed.handled);
    CHECK(decodeResponse(encodeResponse(failed)).value().id == create.id);
    CHECK(engine.created == 32);
    CHECK(engine.createAttempts == 33);
    return 0;
}

int testUnavailableSequences() {
    FakeEngine engine;
    engine.available = false;
    BrokerConnection broker(engine);
    Request health = request(Operation::Health, 5);
    CHECK(broker.dispatch(health).status == Status::Unavailable);
    CHECK(broker.dispatch(health).status == Status::Stale);
    CHECK(broker.dispatch(request(Operation::CreateSession, 4, 0, 9)).status == Status::Stale);
    CHECK(engine.createAttempts == 0);

    engine.available = true;
    Request create = request(Operation::CreateSession, 6, 0, 9);
    const Response created = broker.dispatch(create);
    CHECK(created.status == Status::Ok);
    engine.available = false;
    Request key = request(Operation::ProcessKey, 8, created.id.sessionId, 9);
    key.keycode = 'a';
    const Response unavailable = broker.dispatch(key);
    CHECK(unavailable.status == Status::Unavailable);
    CHECK(unavailable.id == key.id);
    CHECK(broker.dispatch(key).status == Status::Stale);
    CHECK(engine.processed == 0);
    engine.available = true;
    CHECK(broker.dispatch(key).status == Status::Stale);
    key.id.sequence = 7;
    CHECK(broker.dispatch(key).status == Status::Stale);
    key.id.sequence = 9;
    key.id.connectionEpoch = 78;
    CHECK(broker.dispatch(key).status == Status::Stale);
    key.id.connectionEpoch = 77;
    CHECK(broker.dispatch(key).status == Status::Ok);
    CHECK(engine.processed == 1);
    CHECK(broker.dispatch(key).status == Status::Stale);
    CHECK(engine.processed == 1);
    Request candidate = request(Operation::SelectCandidate, 10, created.id.sessionId, 9);
    candidate.candidateIndex = static_cast<std::uint32_t>(kMaxCandidates);
    candidate.snapshotRevision = 1;
    CHECK(broker.dispatch(candidate).status == Status::InvalidRequest);
    CHECK(engine.selected == 0);
    CHECK(broker.dispatch(candidate).status == Status::Stale);
    return 0;
}

int testPermanentFailure() {
    for (unsigned fault = 0; fault != 5; ++fault) {
        FakeEngine engine;
        BrokerConnection broker(engine);
        const Response first = broker.dispatch(request(Operation::CreateSession, 1, 0, 9));
        const Response second = broker.dispatch(request(Operation::CreateSession, 2, 0, 10));
        CHECK(first.status == Status::Ok);
        CHECK(second.status == Status::Ok);
        Request failing = request(Operation::ProcessKey, 3, first.id.sessionId, 9);
        failing.keycode = 'a';
        if (fault == 0) {
            engine.throwOnReady = true;
            failing = request(Operation::Health, 3);
        } else if (fault == 1) {
            engine.throwOnCreate = true;
            failing = request(Operation::CreateSession, 3, 0, 11);
        } else if (fault == 2) {
            engine.throwOnProcess = true;
        } else if (fault == 3) {
            engine.throwOnSnapshot = true;
        } else {
            engine.throwOnDestroy = true;
            failing.operation = Operation::DestroySession;
        }
        const Response failed = broker.dispatch(failing);
        CHECK(failed.status == Status::EngineFailure);
        CHECK(failed.id == failing.id);
        CHECK(!failed.handled);
        CHECK(decodeResponse(encodeResponse(failed)).value().id == failing.id);
        CHECK(engine.destroyAttempts == (fault == 4 ? 3u : 2u));
        if (fault != 4) CHECK(engine.states.empty());
        engine.throwOnReady = false;
        engine.throwOnCreate = false;
        engine.throwOnProcess = false;
        engine.throwOnSnapshot = false;
        engine.throwOnDestroy = false;
        const std::size_t readyCalls = engine.readyCalls;
        const std::size_t createAttempts = engine.createAttempts;
        CHECK(broker.dispatch(request(Operation::Health, 4)).status == Status::Unavailable);
        CHECK(broker.dispatch(request(Operation::CreateSession, 5, 0, 11)).status == Status::Unavailable);
        Request newEpoch = request(Operation::CreateSession, 1, 0, 11);
        newEpoch.id.connectionEpoch = 78;
        const Response rejected = broker.dispatch(newEpoch);
        CHECK(rejected.status == Status::Unavailable);
        CHECK(rejected.id == newEpoch.id);
        CHECK(engine.created == 2);
        CHECK(engine.readyCalls == readyCalls);
        CHECK(engine.createAttempts == createAttempts);
    }
    return 0;
}

int testBroker() {
    FakeEngine engine;
    BrokerConnection broker(engine);

    Request create = request(Operation::CreateSession, 1, 0, 42);
    const Response created = broker.dispatch(create);
    CHECK(created.status == Status::Ok);
    CHECK(created.id.sessionId != 0);
    CHECK(created.id.sequence == create.id.sequence);
    CHECK(created.id.connectionEpoch == create.id.connectionEpoch);
    CHECK(created.id.contextEpoch == 42);

    Request key = request(Operation::ProcessKey, 2, created.id.sessionId, 42);
    key.keycode = 'a';
    const Response processed = broker.dispatch(key);
    CHECK(processed.status == Status::Ok);
    CHECK(processed.handled);
    CHECK(processed.id == key.id);
    CHECK(engine.processed == 1);
    CHECK(processed.snapshot.rawInput == "a");
    CHECK(processed.snapshot.revision == 1);

    const Response duplicate = broker.dispatch(key);
    CHECK(duplicate.status == Status::Stale);
    CHECK(engine.processed == 1);
    Request lowerSequence = request(Operation::ProcessKey, 1, created.id.sessionId, 42);
    lowerSequence.keycode = 'z';
    CHECK(broker.dispatch(lowerSequence).status == Status::Stale);
    CHECK(engine.processed == 1);

    Request secondCreate = request(Operation::CreateSession, 3, 0, 84);
    const Response second = broker.dispatch(secondCreate);
    CHECK(second.status == Status::Ok);
    CHECK(second.id.sessionId != created.id.sessionId);

    Request crossSession = request(Operation::ProcessKey, 4, second.id.sessionId, 42);
    crossSession.keycode = 'b';
    CHECK(broker.dispatch(crossSession).status == Status::Stale);
    CHECK(engine.processed == 1);

    Request candidate = request(Operation::SelectCandidate, 5, created.id.sessionId, 42);
    candidate.candidateIndex = 0;
    candidate.snapshotRevision = processed.snapshot.revision - 1;
    CHECK(broker.dispatch(candidate).status == Status::Stale);
    CHECK(engine.selected == 0);

    candidate.snapshotRevision = processed.snapshot.revision;
    candidate.id.sequence = 6;
    CHECK(broker.dispatch(candidate).status == Status::Ok);
    CHECK(engine.selected == 1);

    Request wrongEpoch = request(Operation::Cancel, 7, created.id.sessionId, 41);
    CHECK(broker.dispatch(wrongEpoch).status == Status::Stale);

    engine.throwOnProcess = true;
    Request failing = request(Operation::ProcessKey, 8, second.id.sessionId, 84);
    failing.keycode = 'c';
    CHECK(broker.dispatch(failing).status == Status::EngineFailure);
    CHECK(engine.states.empty());
    Request afterFailureNewEpoch = request(Operation::CreateSession, 1, 0, 7);
    afterFailureNewEpoch.id.connectionEpoch = 78;
    CHECK(broker.dispatch(afterFailureNewEpoch).status == Status::Unavailable);
    CHECK(engine.created == 2);

    FakeEngine unhandledEngine;
    BrokerConnection unhandledBroker(unhandledEngine);
    const Response unhandledCreated = unhandledBroker.dispatch(request(Operation::CreateSession, 1, 0, 9));
    CHECK(unhandledCreated.status == Status::Ok);
    const std::uint64_t engineSession = unhandledEngine.states.begin()->first;
    unhandledEngine.states.at(engineSession).raw = "ni";
    unhandledEngine.states.at(engineSession).commit = "pending";
    unhandledEngine.handleProcess = false;
    Request unhandledKey = request(Operation::ProcessKey, 2, unhandledCreated.id.sessionId, 9);
    unhandledKey.keycode = 'x';
    const Response unhandled = unhandledBroker.dispatch(unhandledKey);
    CHECK(unhandled.status == Status::Ok);
    CHECK(!unhandled.handled);
    CHECK(unhandled.id == unhandledKey.id);
    CHECK(unhandled.snapshot.commitText.empty());
    CHECK(unhandled.snapshot.rawInput == "ni");
    CHECK(unhandled.snapshot.preedit == "ni");
    CHECK(unhandled.snapshot.caretUtf8Offset == 2);
    CHECK(unhandled.snapshot.candidates.size() == 2);
    CHECK(unhandled.snapshot.revision == 0);
    CHECK(unhandledEngine.snapshotCalls == 2);
    CHECK(unhandledEngine.states.at(engineSession).commit == "pending");
    CHECK(unhandledBroker.dispatch(unhandledKey).status == Status::Stale);
    CHECK(unhandledEngine.processed == 1);
    CHECK(unhandledEngine.snapshotCalls == 2);
    CHECK(unhandledEngine.states.at(engineSession).commit == "pending");
    const auto decodedUnhandled = decodeResponse(encodeResponse(unhandled));
    CHECK(decodedUnhandled.has_value());
    CHECK(decodedUnhandled->snapshot.rawInput == "ni");
    CHECK(decodedUnhandled->snapshot.preedit == "ni");
    CHECK(decodedUnhandled->snapshot.commitText.empty());

    unhandledKey.id.sequence = 3;
    const Response repeatedUnhandled = unhandledBroker.dispatch(unhandledKey);
    CHECK(repeatedUnhandled.status == Status::Ok);
    CHECK(!repeatedUnhandled.handled);
    CHECK(repeatedUnhandled.snapshot.rawInput == "ni");
    CHECK(repeatedUnhandled.snapshot.commitText.empty());
    CHECK(repeatedUnhandled.snapshot.revision == 0);
    CHECK(unhandledEngine.states.at(engineSession).commit == "pending");

    unhandledEngine.handleProcess = true;
    Request handledKey = request(Operation::ProcessKey, 4, unhandledCreated.id.sessionId, 9);
    handledKey.keycode = 'y';
    const Response handledAfterUnhandled = unhandledBroker.dispatch(handledKey);
    CHECK(handledAfterUnhandled.status == Status::Ok);
    CHECK(handledAfterUnhandled.handled);
    CHECK(handledAfterUnhandled.snapshot.commitText == "pending");
    CHECK(handledAfterUnhandled.snapshot.rawInput == "niy");
    CHECK(handledAfterUnhandled.snapshot.revision == 1);
    CHECK(unhandledEngine.snapshotCalls == 4);
    CHECK(unhandledEngine.states.at(engineSession).commit.empty());
    handledKey.id.sequence = 5;
    const Response afterDrained = unhandledBroker.dispatch(handledKey);
    CHECK(afterDrained.handled);
    CHECK(afterDrained.snapshot.commitText.empty());
    CHECK(afterDrained.snapshot.revision == 2);

    unhandledEngine.states.at(engineSession).commit = "candidate-pending";
    Request rejectedCandidate = request(Operation::SelectCandidate, 6, unhandledCreated.id.sessionId, 9);
    rejectedCandidate.candidateIndex = 2;
    rejectedCandidate.snapshotRevision = afterDrained.snapshot.revision;
    const Response candidateUnhandled = unhandledBroker.dispatch(rejectedCandidate);
    CHECK(candidateUnhandled.status == Status::Ok);
    CHECK(!candidateUnhandled.handled);
    CHECK(candidateUnhandled.snapshot.commitText.empty());
    CHECK(candidateUnhandled.snapshot.rawInput == "niyy");
    CHECK(candidateUnhandled.snapshot.preedit == "niyy");
    CHECK(candidateUnhandled.snapshot.caretUtf8Offset == 4);
    CHECK(candidateUnhandled.snapshot.candidates.size() == 2);
    CHECK(candidateUnhandled.snapshot.revision == afterDrained.snapshot.revision);
    CHECK(unhandledEngine.states.at(engineSession).commit == "candidate-pending");
    handledKey.id.sequence = 7;
    const Response handledAfterCandidate = unhandledBroker.dispatch(handledKey);
    CHECK(handledAfterCandidate.status == Status::Ok);
    CHECK(handledAfterCandidate.handled);
    CHECK(handledAfterCandidate.snapshot.commitText == "candidate-pending");
    CHECK(handledAfterCandidate.snapshot.revision == 3);
    CHECK(unhandledEngine.states.at(engineSession).commit.empty());
    handledKey.id.sequence = 8;
    const Response afterCandidateDrained = unhandledBroker.dispatch(handledKey);
    CHECK(afterCandidateDrained.handled);
    CHECK(afterCandidateDrained.snapshot.commitText.empty());
    CHECK(afterCandidateDrained.snapshot.revision == 4);

    FakeEngine unavailable;
    unavailable.available = false;
    BrokerConnection unavailableBroker(unavailable);
    CHECK(unavailableBroker.dispatch(request(Operation::Health, 1)).status == Status::Unavailable);

    Request afterFailure = request(Operation::Health, 9);
    CHECK(broker.dispatch(afterFailure).status == Status::Unavailable);

    FakeEngine disconnectEngine;
    {
        BrokerConnection disconnecting(disconnectEngine);
        const Response one = disconnecting.dispatch(request(Operation::CreateSession, 1, 0, 1));
        CHECK(one.status == Status::Ok);
        CHECK(disconnectEngine.states.size() == 1);
    }
    CHECK(disconnectEngine.states.empty());
    CHECK(disconnectEngine.destroyed == 1);

    FakeEngine limitedEngine;
    {
        BrokerConnection limited(limitedEngine);
        for (std::uint64_t sequence = 1; sequence <= 32; ++sequence) {
            const Response session = limited.dispatch(request(Operation::CreateSession, sequence, 0, sequence));
            CHECK(session.status == Status::Ok);
        }
        CHECK(limited.dispatch(request(Operation::CreateSession, 33, 0, 33)).status == Status::Busy);
    }
    CHECK(limitedEngine.created == 32);
    CHECK(limitedEngine.destroyed == 32);
    return 0;
}
} // namespace

int main() {
    if (const int result = testUnicodeAndCodec(); result != 0) return result;
    if (const int result = testCandidateAppearance(); result != 0) return result;
    if (const int result = testDecoderLimits(); result != 0) return result;
    if (const int result = testModifierSideEffects(); result != 0) return result;
    if (const int result = testCreateFailureIdentity(); result != 0) return result;
    if (const int result = testUnavailableSequences(); result != 0) return result;
    if (const int result = testPermanentFailure(); result != 0) return result;
    if (const int result = testBroker(); result != 0) return result;
    if (const int result = testUnhandledSnapshotFailure(); result != 0) return result;
    std::cout << "ProtocolTests passed\n";
    return 0;
}
