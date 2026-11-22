#pragma once

#include "Pipe.h"

namespace xmjd::win::detail {

struct PipeTokenContext {
    DWORD integrity = 0;
    bool restricted = false;
    bool appContainer = false;
    bool elevated = false;
};

// This check is directional. Only an ordinary Host may serve input; a High
// client never becomes a server. SID/session equality is checked by the caller.
constexpr std::optional<PipeClientKind> classifyPipeClient(
    const PipeTokenContext& server, const PipeTokenContext& client, bool sameLogon) noexcept {
    if (!sameLogon || server.restricted || server.appContainer || server.elevated ||
        server.integrity < SECURITY_MANDATORY_MEDIUM_RID ||
        server.integrity >= SECURITY_MANDATORY_HIGH_RID || client.restricted) return std::nullopt;
    if (client.appContainer) {
        return client.elevated ? std::nullopt : std::optional{PipeClientKind::AppContainer};
    }
    if (client.elevated && client.integrity == SECURITY_MANDATORY_HIGH_RID) return PipeClientKind::Elevated;
    if (!client.elevated && client.integrity == server.integrity) return PipeClientKind::Desktop;
    return std::nullopt;
}

} // namespace xmjd::win::detail
