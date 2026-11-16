#pragma once

#include "../Protocol/Protocol.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "rime_api.h"

namespace xmjd::win {
class RimeRuntime final : public Engine {
public:
    explicit RimeRuntime(std::filesystem::path executableDirectory);
    ~RimeRuntime() override;
    RimeRuntime(const RimeRuntime&) = delete;
    RimeRuntime& operator=(const RimeRuntime&) = delete;

    bool initialize(bool explicitDeploy, int& errorCode);
    bool ready() const override;
    std::uint64_t createSession() override;
    void destroySession(std::uint64_t session) override;
    bool processKey(std::uint64_t session, std::uint32_t key, std::uint32_t modifiers) override;
    bool selectCandidate(std::uint64_t session, std::uint32_t pageIndex) override;
    void finalize(std::uint64_t session, bool convert) override;
    void cancel(std::uint64_t session) override;
    Snapshot snapshot(std::uint64_t session, bool drainCommit = true) override;

private:
    struct Session {
        RimeSessionId id = 0;
        std::string pendingRawCommit;
    };

    bool loadLibrary(int& errorCode);
    bool publishResources(bool explicitDeploy, bool& pendingInitialDeploy, int& errorCode);
    bool recoverResourceJournal(bool& pendingInitialDeploy, int& errorCode);
    bool validateSchemas(int& errorCode);
    bool runDeployment(int& errorCode);
    Session* findSession(std::uint64_t session);
    bool sessionHealthy(RimeSessionId session, const char* expectedSchema = nullptr);
    std::vector<std::pair<std::string, std::string>> schemaList();

    std::filesystem::path executableDirectory_;
    std::filesystem::path rootDirectory_;
    std::filesystem::path sharedDirectory_;
    std::filesystem::path userDirectory_;
    std::filesystem::path stagingDirectory_;
    std::filesystem::path backupDirectory_;
    std::string sharedDirectoryUtf8_;
    std::string userDirectoryUtf8_;
    std::string stagingDirectoryUtf8_;
    std::string distributionName_ = "XMJD";
    std::string distributionCodeName_ = "xmjd";
    std::string appName_ = "rime.xmjd";
    std::string logDirectoryUtf8_;
    RimeTraits traits_{};
    HMODULE library_ = nullptr;
    RimeApi* api_ = nullptr;
    bool initialized_ = false;
    bool ready_ = false;
    std::uint64_t nextSession_ = 1;
    std::unordered_map<std::uint64_t, Session> sessions_;
};
} // namespace xmjd::win
