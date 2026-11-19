#include "Broker.h"

#include <limits>
#include <utility>

namespace xmjd {
namespace {
constexpr std::size_t kMaxSessionsPerConnection = 32;

Response resultFor(const Request& request, Status status) {
    Response response;
    response.id = request.id;
    response.id.version = kProtocolVersion;
    response.status = status;
    return response;
}

bool isSessionOperation(Operation operation) noexcept {
    return operation == Operation::ProcessKey || operation == Operation::SelectCandidate || operation == Operation::Finalize ||
           operation == Operation::Cancel || operation == Operation::DestroySession;
}

bool isMutation(Operation operation) noexcept {
    return operation == Operation::ProcessKey || operation == Operation::SelectCandidate || operation == Operation::Finalize ||
           operation == Operation::Cancel || operation == Operation::DestroySession;
}
} // namespace

BrokerConnection::~BrokerConnection() {
    for (const auto& entry : sessions_) {
        try {
            engine_.destroySession(entry.second.engineId);
        } catch (...) {
        }
    }
    sessions_.clear();
}

Response BrokerConnection::dispatch(const Request& request) {
    try {
        if (failed_ || nextSession_ == 0) return resultFor(request, Status::Unavailable);
        if (request.id.version != kProtocolVersion) return resultFor(request, Status::UnsupportedVersion);
        if (request.id.connectionEpoch == 0) return resultFor(request, Status::InvalidRequest);
        if (epoch_ == 0) epoch_ = request.id.connectionEpoch;
        if (request.id.connectionEpoch != epoch_) return resultFor(request, Status::Stale);
        if (request.id.sequence <= lastSequence_) return resultFor(request, Status::Stale);
        lastSequence_ = request.id.sequence;

        if (request.operation == Operation::Health) {
            if (request.id.sessionId != 0 || request.id.contextEpoch != 0) return resultFor(request, Status::InvalidRequest);
            const bool ready = engine_.ready();
            Response response = resultFor(request, ready ? Status::Ok : Status::Unavailable);
            response.handled = ready;
            return response;
        }
        if (!engine_.ready()) return resultFor(request, Status::Unavailable);

        if (request.operation == Operation::CreateSession) {
            if (request.id.sessionId != 0 || request.id.contextEpoch == 0 ||
                (!sessions_.empty() && sessions_.size() >= kMaxSessionsPerConnection)) {
                return resultFor(request, sessions_.size() >= kMaxSessionsPerConnection ? Status::Busy : Status::InvalidRequest);
            }
            const std::uint64_t engineId = engine_.createSession();
            if (engineId == 0) return resultFor(request, Status::EngineFailure);
            const std::uint64_t sessionId = nextSession_++;
            try {
                sessions_.emplace(sessionId, Session{engineId, request.id.contextEpoch, 0});
            } catch (...) {
                try {
                    engine_.destroySession(engineId);
                } catch (...) {
                }
                throw;
            }
            Response response = resultFor(request, Status::Ok);
            response.id.sessionId = sessionId;
            response.handled = true;
            response.snapshot.asciiMode = engine_.snapshot(engineId, false).asciiMode;
            return response;
        }

        if (!isSessionOperation(request.operation) || request.id.sessionId == 0) return resultFor(request, Status::InvalidRequest);
        const auto sessionIt = sessions_.find(request.id.sessionId);
        if (sessionIt == sessions_.end()) return resultFor(request, Status::Stale);
        Session& session = sessionIt->second;
        if (request.id.contextEpoch != session.contextEpoch) return resultFor(request, Status::Stale);
        if (request.operation == Operation::SelectCandidate) {
            if (request.candidateIndex >= kMaxCandidates) return resultFor(request, Status::InvalidRequest);
            if (request.snapshotRevision != session.revision) return resultFor(request, Status::Stale);
        }

        if (request.operation == Operation::DestroySession) {
            engine_.destroySession(session.engineId);
            sessions_.erase(sessionIt);
            Response response = resultFor(request, Status::Ok);
            response.handled = true;
            return response;
        }

        if (session.revision == std::numeric_limits<std::uint64_t>::max()) return resultFor(request, Status::EngineFailure);
        if (request.operation == Operation::ProcessKey) {
            if (!engine_.processKey(session.engineId, request.keycode, request.modifiers)) {
                Response response = resultFor(request, Status::Ok);
                // Rime's ascii_composer can commit/toggle on a modifier release
                // while returning kNoop. Preserve that output without eating the
                // native modifier event. Ordinary rejected keys do not drain.
                const bool modifier = request.keycode >= 0xffe1 && request.keycode <= 0xffee;
                if (modifier) ++session.revision;
                response.snapshot = engine_.snapshot(session.engineId, modifier);
                response.snapshot.revision = session.revision;
                response.handled = false;
                return response;
            }
        } else if (request.operation == Operation::SelectCandidate) {
            if (!engine_.selectCandidate(session.engineId, request.candidateIndex)) {
                Response response = resultFor(request, Status::Ok);
                response.snapshot = engine_.snapshot(session.engineId, false);
                response.snapshot.revision = session.revision;
                response.handled = false;
                return response;
            }
        } else if (request.operation == Operation::Finalize) {
            engine_.finalize(session.engineId, request.convert);
        } else if (request.operation == Operation::Cancel) {
            engine_.cancel(session.engineId);
        } else {
            return resultFor(request, Status::InvalidRequest);
        }

        if (isMutation(request.operation)) {
            if (session.revision == std::numeric_limits<std::uint64_t>::max()) return resultFor(request, Status::EngineFailure);
            ++session.revision;
        }
        Snapshot snapshot = engine_.snapshot(session.engineId);
        snapshot.revision = session.revision;
        Response response = resultFor(request, Status::Ok);
        response.snapshot = std::move(snapshot);
        response.handled = true;
        return response;
    } catch (...) {
        failed_ = true;
        for (const auto& entry : sessions_) {
            try {
                engine_.destroySession(entry.second.engineId);
            } catch (...) {
            }
        }
        sessions_.clear();
        return resultFor(request, Status::EngineFailure);
    }
}
} // namespace xmjd
