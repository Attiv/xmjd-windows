#pragma once
#include "Protocol.h"
#include <unordered_map>

namespace xmjd {
class BrokerConnection {
public:
    explicit BrokerConnection(Engine& engine) : engine_(engine) {}
    ~BrokerConnection();
    BrokerConnection(const BrokerConnection&) = delete;
    BrokerConnection& operator=(const BrokerConnection&) = delete;
    Response dispatch(const Request& request);
private:
    struct Session { std::uint64_t engineId; std::uint64_t contextEpoch; std::uint64_t revision = 0; };
    Engine& engine_;
    std::uint64_t epoch_ = 0;
    std::uint64_t lastSequence_ = 0;
    std::uint64_t nextSession_ = 1;
    bool failed_ = false;
    std::unordered_map<std::uint64_t, Session> sessions_;
};
} // namespace xmjd
