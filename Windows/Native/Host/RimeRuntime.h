#pragma once

#include "../Protocol/Protocol.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "rime_api.h"
#include "Correction/ContextualCorrection.h"

namespace xmjd::win {
namespace detail {
bool writeSettingsSchemaFile(const std::filesystem::path& path, std::string_view schema);
}

class RimeRuntime final : public Engine {
public:
    RimeRuntime(std::filesystem::path executableDirectory, std::filesystem::path dataDirectory);
    ~RimeRuntime() override;
    RimeRuntime(const RimeRuntime&) = delete;
    RimeRuntime& operator=(const RimeRuntime&) = delete;

    bool initialize(bool explicitDeploy, int& errorCode);
    // Rebuilds Rime's compiled data from the current user configuration. Active
    // sessions are destroyed; clients must create fresh sessions afterwards.
    bool redeploy(int& errorCode);
    bool ready() const override;
    std::uint64_t createSession() override;
    void destroySession(std::uint64_t session) override;
    bool processKey(std::uint64_t session, std::uint32_t key, std::uint32_t modifiers) override;
    bool selectCandidate(std::uint64_t session, std::uint32_t pageIndex) override;
    void finalize(std::uint64_t session, bool convert) override;
    void cancel(std::uint64_t session) override;
    Snapshot snapshot(std::uint64_t session, bool drainCommit = true) override;
    bool correctionAvailable(std::uint64_t session) override;
    void setCorrectionContext(std::uint64_t session, std::string_view text) override;
    std::optional<Snapshot> refreshCandidates(std::uint64_t session) override;

private:
    struct Session {
        RimeSessionId id = 0;
        std::string pendingRawCommit;
        std::uint64_t correctionEpoch = 0;
        std::string precedingText;
        std::string pendingPool;
        std::future<std::string> correction;
        bool singleCharacter = false;
        std::string shortCode;
        ULONGLONG shortReadyAt = 0;
    };

    bool loadLibrary(int& errorCode);
    void stopRime() noexcept;
    bool publishResources(bool explicitDeploy, bool& pendingInitialDeploy, int& errorCode);
    bool recoverResourceJournal(bool& pendingInitialDeploy, int& errorCode);
    bool validateSchemas(int& errorCode);
    bool runDeployment(int& errorCode);
    Session* findSession(std::uint64_t session);
    bool sessionHealthy(RimeSessionId session, const char* expectedSchema = nullptr);
    std::vector<std::pair<std::string, std::string>> schemaList();
    std::filesystem::path settingsPath() const;
    std::string readSettingsValue(std::string_view key) const;
    std::string readSettingsSchema() const;
    bool resolveSchemeSource(std::string_view configured, std::filesystem::path& resolved) const;
    bool writeSettingsSchema(const std::string& schema);
    bool loadSelectedSchema();
    void rememberSessionSchema(RimeSessionId session);
    bool probeUpToDate() const;
    void writeEngineProbe();
    void prepareCorrection() noexcept;
    bool correctionEligible(const Session&, const Snapshot&) const noexcept;
    void cancelShortCorrection(Session&) noexcept;
    void queueCorrection(Session&, Snapshot&) noexcept;
    bool submitCorrectionPool(Session&, std::string_view rawInput);

    std::filesystem::path executableDirectory_;
    std::filesystem::path rootDirectory_;
    std::filesystem::path sharedDirectory_;
    std::filesystem::path userDirectory_;
    std::filesystem::path stagingDirectory_;
    std::filesystem::path compiledDirectory_;
    std::filesystem::path backupDirectory_;
    bool usingExternalSource_ = false;
    std::string sharedDirectoryUtf8_;
    std::string userDirectoryUtf8_;
    std::string stagingDirectoryUtf8_;
    std::string distributionName_ = "XMJD";
    std::string distributionCodeName_ = "xmjd";
    std::string appName_ = "rime.xmjd";
    std::string logDirectoryUtf8_;
    std::string selectedSchema_ = "xmjd6";
    RimeTraits traits_{};
    HMODULE library_ = nullptr;
    RimeApi* api_ = nullptr;
    bool initialized_ = false;
    bool ready_ = false;
    std::uint64_t nextSession_ = 1;
    std::unordered_map<std::uint64_t, Session> sessions_;
    std::unordered_set<std::string> correctionSchemas_;
    std::unique_ptr<correction::Worker> correctionWorker_;
};
} // namespace xmjd::win
