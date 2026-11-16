#include "RimeRuntime.h"
#include "../Common/Pipe.h"
#include "Version.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <limits>
#include <memory>
#include <optional>
#include <string_view>
#include <system_error>
#include <objbase.h>
#include <windows.h>

namespace xmjd::win {
namespace {
constexpr int kRuntimeErrorPaths = 10;
constexpr int kRuntimeErrorResources = 11;
constexpr int kRuntimeErrorJournal = 12;
constexpr int kRuntimeErrorLibrary = 13;
constexpr int kRuntimeErrorLua = 14;
constexpr int kRuntimeErrorDeployment = 15;
constexpr int kRuntimeErrorSchema = 16;
constexpr int kRuntimeErrorSession = 17;

std::string trimAscii(std::string_view value) {
    std::size_t begin = 0;
    std::size_t end = value.size();
    while (begin < end && (value[begin] == ' ' || value[begin] == '\t')) ++begin;
    while (end > begin && (value[end - 1] == ' ' || value[end - 1] == '\t' || value[end - 1] == '\r')) --end;
    return std::string(value.substr(begin, end - begin));
}

bool validSchemaId(std::string_view id) {
    if (id.empty() || id.size() > 64) return false;
    for (const unsigned char byte : id) {
        if (!(std::isalnum(byte) || byte == '.' || byte == '_' || byte == '-')) return false;
    }
    return true;
}

bool writeFile(const std::filesystem::path& path, std::string_view value) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = value.size() <= std::numeric_limits<DWORD>::max() &&
                    WriteFile(file, value.data(), static_cast<DWORD>(value.size()), &written, nullptr) &&
                    written == value.size() && FlushFileBuffers(file);
    CloseHandle(file);
    return ok;
}

std::optional<std::string> readSmallFile(const std::filesystem::path& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return std::nullopt;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 || size.QuadPart > 1024) {
        CloseHandle(file);
        return std::nullopt;
    }
    std::string value(static_cast<std::size_t>(size.QuadPart), '\0');
    DWORD read = 0;
    const bool ok = value.empty() || (ReadFile(file, value.data(), static_cast<DWORD>(value.size()), &read, nullptr) &&
                                      read == value.size());
    CloseHandle(file);
    if (!ok) return std::nullopt;
    return value;
}

bool writeJournal(const std::filesystem::path& journal, std::string_view phase,
                  std::string_view backupName) {
    const auto temporary = journal.wstring() + L".tmp";
    const std::string body = std::string(phase) + "\n" + std::string(backupName) + "\n";
    if (!writeFile(temporary, body)) return false;
    return MoveFileExW(temporary.c_str(), journal.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

bool hasFile(const std::filesystem::path& path) {
    std::error_code ec;
    return std::filesystem::is_regular_file(path, ec) && !ec;
}

bool hasDirectory(const std::filesystem::path& path) {
    std::error_code ec;
    return std::filesystem::is_directory(path, ec) && !ec;
}

bool hasPublishedMarker(const std::filesystem::path& sharedDirectory) {
    return hasFile(sharedDirectory / L".xmjd-resource-ready");
}

std::string utf8(const std::filesystem::path& path) {
    const auto value = path.u8string();
    return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}

bool copyResources(const std::filesystem::path& source, const std::filesystem::path& destination) {
    std::error_code ec;
    if (!hasDirectory(source) || !std::filesystem::create_directories(destination, ec) || ec) return false;

    for (std::filesystem::recursive_directory_iterator it(source, ec), end; it != end && !ec; it.increment(ec)) {
        const auto status = it->symlink_status(ec);
        if (ec || std::filesystem::is_symlink(status)) return false;
        const auto relative = std::filesystem::relative(it->path(), source, ec);
        if (ec || relative.empty() || relative.is_absolute()) return false;
        const auto target = destination / relative;
        if (std::filesystem::is_directory(status)) {
            std::filesystem::create_directories(target, ec);
        } else if (std::filesystem::is_regular_file(status)) {
            std::filesystem::create_directories(target.parent_path(), ec);
            if (!ec) std::filesystem::copy_file(it->path(), target, std::filesystem::copy_options::none, ec);
        } else {
            return false;
        }
        if (ec) return false;
    }
    return !ec;
}

bool validateResourceTree(const std::filesystem::path& directory) {
    // A schema can load while a lazy Lua/OpenCC path is still missing. Reject
    // incomplete packages before publishing readiness, including on recovery.
    for (const auto* relative : {
             L"xmjd6.schema.yaml", L"default.yaml", L"default.custom.yaml", L"essay.txt",
             L"xmjd6.extended.dict.yaml", L"pinyin_simp.dict.yaml", L"liangfen.dict.yaml",
             L"lua/xmjd6_processor.lua", L"lua/xmjd6_opencc_filter.lua",
             L"lua/zzc/xmjd6_zzc_processor.lua", L"zzc_state/char_parts.tsv",
             L"opencc/s2t.json", L"opencc/t2s.json",
             L"opencc/STCharacters.ocd2", L"opencc/STPhrases.ocd2",
             L"opencc/TSCharacters.ocd2", L"opencc/TSPhrases.ocd2",
             L"opencc/Data/xmjd6_s2g_chars.lua", L"opencc/Data/xmjd6_s2g_phrases_index.lua",
             L"opencc/Data/xmjd6_s2tw_chars.lua", L"opencc/Data/xmjd6_s2tw_phrases_index.lua",
             L"opencc/Data/xmjd6_emoji_chars.lua", L"opencc/Data/xmjd6_emoji_phrases_index.lua",
             L"opencc/Data/xmjd6_martian_chars.lua", L"opencc/Data/xmjd6_martian_phrases_index.lua"}) {
        if (!hasFile(directory / relative)) return false;
    }
    for (const auto* dataset : {L"xmjd6_s2g", L"xmjd6_s2tw", L"xmjd6_emoji", L"xmjd6_martian"}) {
        for (const wchar_t shard : std::wstring_view(L"0123456789abcdef")) {
            const auto filename = std::wstring(dataset) + L"_phrases_" + shard + L".lua";
            if (!hasFile(directory / L"opencc" / L"Data" / filename)) return false;
        }
    }
    return true;
}


std::wstring uniqueSuffix() {
    static std::atomic<std::uint64_t> counter{0};
    const std::uint64_t sequence = counter.fetch_add(1, std::memory_order_relaxed);
    return std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()) + L"-" +
           std::to_wstring(sequence);
}

bool parseJournal(const std::string& contents, std::string& phase, std::string& backupName) {
    const auto first = contents.find('\n');
    const auto second = first == std::string::npos ? first : contents.find('\n', first + 1);
    if (first == std::string::npos || second == std::string::npos || second + 1 != contents.size()) return false;
    phase = contents.substr(0, first);
    backupName = contents.substr(first + 1, second - first - 1);
    if (phase != "prepared" && phase != "old-moved" && phase != "new-moved") return false;
    if (!backupName.empty() && backupName != "-" &&
        (backupName.find_first_of("/\\:") != std::string::npos || backupName.rfind("ResourceRollback-", 0) != 0)) return false;
    return backupName == "-" || (!backupName.empty() && backupName.size() < 80);
}

std::string selectionLabel(const RimeContext& context, int index) {
    if (context.select_labels && context.select_labels[index]) return context.select_labels[index];
    if (context.menu.select_keys) {
        const std::string keys(context.menu.select_keys);
        std::size_t offset = 0;
        int current = 0;
        while (offset < keys.size()) {
            const unsigned char lead = static_cast<unsigned char>(keys[offset]);
            const std::size_t width = lead < 0x80 ? 1 : (lead < 0xE0 ? 2 : (lead < 0xF0 ? 3 : 4));
            if (current++ == index) return keys.substr(offset, std::min(width, keys.size() - offset));
            offset += std::min(width, keys.size() - offset);
        }
    }
    return std::to_string(index == 9 ? 0 : index + 1);
}

std::string takeCommit(RimeApi* api, RimeSessionId session) {
    if (!api || !api->get_commit || !api->free_commit) return {};
    RimeCommit commit{};
    RIME_STRUCT_INIT(RimeCommit, commit);
    const std::unique_ptr<RimeCommit, decltype(api->free_commit)> cleanup(&commit, api->free_commit);
    if (!api->get_commit(session, &commit)) return {};
    return commit.text ? commit.text : "";
}
} // namespace

RimeRuntime::RimeRuntime(std::filesystem::path executableDirectory)
    : executableDirectory_(std::move(executableDirectory)) {}

RimeRuntime::~RimeRuntime() {
    ready_ = false;
    if (api_) {
        for (auto& entry : sessions_) {
            if (entry.second.id) api_->destroy_session(entry.second.id);
        }
        sessions_.clear();
        if (initialized_) api_->finalize();
    }
    initialized_ = false;
    api_ = nullptr;
    if (library_) FreeLibrary(library_);
}

bool RimeRuntime::initialize(bool explicitDeploy, int& errorCode) {
    errorCode = 0;
    try {
        rootDirectory_ = dataRoot();
        if (rootDirectory_.empty()) {
            errorCode = kRuntimeErrorPaths;
            return false;
        }
        sharedDirectory_ = rootDirectory_ / L"SharedData" / L"Rime";
        userDirectory_ = rootDirectory_ / L"UserData" / L"Rime";
        stagingDirectory_ = rootDirectory_ / L"Staging";
        backupDirectory_ = rootDirectory_ / L"Backups";
        const auto logDirectory = rootDirectory_ / L"Logs";
        std::error_code ec;
        for (const auto& directory : {sharedDirectory_.parent_path(), userDirectory_,
                                      stagingDirectory_, backupDirectory_, logDirectory}) {
            std::filesystem::create_directories(directory, ec);
            if (ec) {
                errorCode = kRuntimeErrorPaths;
                return false;
            }
        }
        sharedDirectoryUtf8_ = utf8(sharedDirectory_);
        userDirectoryUtf8_ = utf8(userDirectory_);
        stagingDirectoryUtf8_ = utf8(stagingDirectory_ / L"Rime");
        logDirectoryUtf8_ = utf8(logDirectory);
        std::filesystem::create_directories(stagingDirectory_ / L"Rime", ec);
        const auto journal = stagingDirectory_ / L"resource-deploy.journal";
        if (!hasFile(journal) && !hasFile(stagingDirectory_ / L".xmjd-resource-pending") &&
            hasDirectory(sharedDirectory_) && !hasPublishedMarker(sharedDirectory_)) {
            errorCode = kRuntimeErrorResources;
            return false;
        }
        bool recoveredInitialDeploy = false;
        if (ec || !recoverResourceJournal(recoveredInitialDeploy, errorCode)) {
            if (!errorCode) errorCode = kRuntimeErrorJournal;
            return false;
        }

        const auto deployMarker = stagingDirectory_ / L".xmjd-deploy-pending";
        const bool settingsDeployPending = hasFile(deployMarker);
        bool pendingInitialDeploy = recoveredInitialDeploy ||
            hasFile(stagingDirectory_ / L".xmjd-resource-pending") || settingsDeployPending;
        if (!publishResources(explicitDeploy, pendingInitialDeploy, errorCode)) return false;
        if (!loadLibrary(errorCode)) return false;

        RIME_STRUCT_INIT(RimeTraits, traits_);
        traits_.shared_data_dir = sharedDirectoryUtf8_.c_str();
        traits_.user_data_dir = userDirectoryUtf8_.c_str();
        traits_.distribution_name = distributionName_.c_str();
        traits_.distribution_code_name = distributionCodeName_.c_str();
        traits_.distribution_version = XMJD_VERSION_STRING;
        traits_.app_name = appName_.c_str();
        traits_.min_log_level = 2;
        traits_.log_dir = logDirectoryUtf8_.c_str();
        traits_.staging_dir = stagingDirectoryUtf8_.c_str();
        api_->setup(&traits_);
        api_->initialize(&traits_);
        initialized_ = true;

        if (!api_->find_module || api_->find_module("lua") == nullptr) {
            errorCode = kRuntimeErrorLua;
            return false;
        }
        if (pendingInitialDeploy || explicitDeploy) {
            if (!runDeployment(errorCode)) return false;
        }
        if (!validateSchemas(errorCode)) return false;
        if (!loadSelectedSchema()) {
            errorCode = kRuntimeErrorSchema;
            return false;
        }
        if (pendingInitialDeploy) {
            if (!writeFile(sharedDirectory_ / L".xmjd-resource-ready", "xmjd6\n")) {
                errorCode = kRuntimeErrorResources;
                return false;
            }
            if (!DeleteFileW((stagingDirectory_ / L".xmjd-resource-pending").c_str()) &&
                GetLastError() != ERROR_FILE_NOT_FOUND) {
                errorCode = kRuntimeErrorJournal;
                return false;
            }
            const auto journal = stagingDirectory_ / L"resource-deploy.journal";
            if (hasFile(journal) && !DeleteFileW(journal.c_str())) {
                errorCode = kRuntimeErrorJournal;
                return false;
            }
            std::error_code cleanupError;
            std::filesystem::remove_all(stagingDirectory_ / L"ResourceDeploy", cleanupError);
            if (cleanupError) {
                errorCode = kRuntimeErrorResources;
                return false;
            }
            // Best effort: a leftover marker only causes one extra deployment.
            DeleteFileW(deployMarker.c_str());
        }
        ready_ = true;
        return true;
    } catch (const std::filesystem::filesystem_error&) {
        errorCode = kRuntimeErrorPaths;
        return false;
    } catch (...) {
        errorCode = kRuntimeErrorResources;
        return false;
    }
}

bool RimeRuntime::recoverResourceJournal(bool& pendingInitialDeploy, int& errorCode) {
    pendingInitialDeploy = false;
    const auto journal = stagingDirectory_ / L"resource-deploy.journal";
    if (!hasFile(journal)) {
        std::error_code cleanupError;
        std::filesystem::remove_all(stagingDirectory_ / L"ResourceDeploy", cleanupError);
        return !cleanupError;
    }
    const auto contents = readSmallFile(journal);
    std::string phase, backupName;
    if (!contents || !parseJournal(*contents, phase, backupName)) {
        errorCode = kRuntimeErrorJournal;
        return false;
    }

    const auto transaction = stagingDirectory_ / L"ResourceDeploy";
    const auto backup = backupName == "-" ? std::filesystem::path{} : backupDirectory_ / widen(backupName);
    const bool backupExists = !backup.empty() && hasDirectory(backup);
    const bool targetExists = hasDirectory(sharedDirectory_);
    std::error_code ec;

    if (phase == "prepared" && !backupExists) {
        std::filesystem::remove(journal, ec);
        if (ec) { errorCode = kRuntimeErrorJournal; return false; }
        std::filesystem::remove_all(transaction, ec);
        if (targetExists && !hasPublishedMarker(sharedDirectory_)) {
            errorCode = kRuntimeErrorJournal;
            return false;
        }
        pendingInitialDeploy = !targetExists;
        if (pendingInitialDeploy && !writeFile(stagingDirectory_ / L".xmjd-resource-pending", "pending\\n")) {
            errorCode = kRuntimeErrorJournal;
            return false;
        }
        return !ec;
    }

    if (phase == "new-moved" && validateResourceTree(sharedDirectory_)) {
        // Publication finished before deployment failed or was interrupted.
        // Keep the complete tree and resume maintenance; do not recopy it on
        // every activation or discard data that a Lua module may have written.
        if (!writeFile(stagingDirectory_ / L".xmjd-resource-pending", "pending\n")) {
            errorCode = kRuntimeErrorJournal;
            return false;
        }
        std::filesystem::remove(journal, ec);
        if (ec) { errorCode = kRuntimeErrorJournal; return false; }
        std::filesystem::remove_all(transaction, ec);
        pendingInitialDeploy = true;
        return !ec;
    }

    if (targetExists) {
        const auto interrupted = stagingDirectory_ / (L"InterruptedResources-" + uniqueSuffix());
        std::filesystem::rename(sharedDirectory_, interrupted, ec);
        if (ec) { errorCode = kRuntimeErrorJournal; return false; }
    }
    if (backupExists) {
        std::filesystem::rename(backup, sharedDirectory_, ec);
        if (ec) { errorCode = kRuntimeErrorJournal; return false; }
    } else if (backupName != "-") {
        errorCode = kRuntimeErrorJournal;
        return false;
    }
    std::filesystem::remove(journal, ec);
    if (ec) { errorCode = kRuntimeErrorJournal; return false; }
    std::filesystem::remove_all(transaction, ec);
    pendingInitialDeploy = !hasDirectory(sharedDirectory_);
    if (pendingInitialDeploy && !writeFile(stagingDirectory_ / L".xmjd-resource-pending", "pending\\n")) {
        errorCode = kRuntimeErrorJournal;
        return false;
    }
    if (ec) { errorCode = kRuntimeErrorJournal; return false; }
    return true;
}

bool RimeRuntime::publishResources(bool explicitDeploy, bool& pendingInitialDeploy, int& errorCode) {
    const auto source = executableDirectory_ / L"Resources" / L"Rime";
    if (hasDirectory(sharedDirectory_)) {
        if ((!hasPublishedMarker(sharedDirectory_) && !pendingInitialDeploy) || !validateResourceTree(sharedDirectory_)) {
            errorCode = kRuntimeErrorResources;
            return false;
        }
        if (explicitDeploy) pendingInitialDeploy = true;
        return true;
    }
    if (!hasDirectory(source)) { errorCode = kRuntimeErrorResources; return false; }
    pendingInitialDeploy = true;

    if (!validateResourceTree(source)) {
        errorCode = kRuntimeErrorResources;
        return false;
    }
    const auto transaction = stagingDirectory_ / L"ResourceDeploy";
    const auto incoming = transaction / L"Incoming";
    std::error_code ec;
    if (hasDirectory(transaction)) {
        errorCode = kRuntimeErrorJournal;
        return false;
    }
    if (!std::filesystem::create_directories(transaction, ec) || ec || !copyResources(source, incoming) ||
        !validateResourceTree(incoming)) {
        std::filesystem::remove_all(transaction, ec);
        errorCode = kRuntimeErrorResources;
        return false;
    }

    const std::string backupName = "-";
    const auto journal = stagingDirectory_ / L"resource-deploy.journal";
    if (!writeJournal(journal, "prepared", backupName)) {
        errorCode = kRuntimeErrorJournal;
        return false;
    }
    if (backupName != "-") {
        std::filesystem::rename(sharedDirectory_, backupDirectory_ / widen(backupName), ec);
        if (ec) { errorCode = kRuntimeErrorResources; return false; }
    }
    if (!writeJournal(journal, "old-moved", backupName)) { errorCode = kRuntimeErrorJournal; return false; }
    std::filesystem::rename(incoming, sharedDirectory_, ec);
    if (ec) { errorCode = kRuntimeErrorResources; return false; }
    if (!writeJournal(journal, "new-moved", backupName)) { errorCode = kRuntimeErrorJournal; return false; }
    pendingInitialDeploy = true;
    std::filesystem::remove_all(transaction, ec);
    if (ec) { errorCode = kRuntimeErrorResources; return false; }
    return true;
}

bool RimeRuntime::loadLibrary(int& errorCode) {
    const auto path = executableDirectory_ / L"rime.dll";
    library_ = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!library_) { errorCode = kRuntimeErrorLibrary; return false; }
    using GetApi = RimeApi* (__cdecl*)();
    auto getApi = reinterpret_cast<GetApi>(GetProcAddress(library_, "rime_get_api"));
    if (!getApi) { errorCode = kRuntimeErrorLibrary; return false; }
    RimeApi* candidate = getApi();
    if (!candidate || !RIME_API_AVAILABLE(candidate, select_candidate_on_current_page) ||
        !candidate->setup || !candidate->initialize || !candidate->finalize ||
        !candidate->create_session || !candidate->destroy_session || !candidate->process_key || !candidate->get_context ||
        !candidate->free_context || !candidate->get_status || !candidate->free_status || !candidate->get_schema_list ||
        !candidate->free_schema_list || !candidate->select_schema || !candidate->find_session || !candidate->find_module ||
        !candidate->get_input || !candidate->commit_composition || !candidate->clear_composition || !candidate->get_commit ||
        !candidate->free_commit) {
        errorCode = kRuntimeErrorLibrary;
        return false;
    }
    api_ = candidate;
    return true;
}

bool RimeRuntime::runDeployment(int& errorCode) {
    if (!api_->start_maintenance || !api_->join_maintenance_thread || !api_->is_maintenance_mode) {
        errorCode = kRuntimeErrorDeployment;
        return false;
    }
    if (api_->is_maintenance_mode()) { errorCode = kRuntimeErrorDeployment; return false; }
    const bool started = api_->start_maintenance(1) != 0;
    api_->join_maintenance_thread();
    if (!started || api_->is_maintenance_mode()) { errorCode = kRuntimeErrorDeployment; return false; }
    return true;
}

std::vector<std::pair<std::string, std::string>> RimeRuntime::schemaList() {
    std::vector<std::pair<std::string, std::string>> result;
    if (!api_ || !api_->get_schema_list || !api_->free_schema_list) return result;
    RimeSchemaList list{};
    const std::unique_ptr<RimeSchemaList, decltype(api_->free_schema_list)> cleanup(&list, api_->free_schema_list);
    if (!api_->get_schema_list(&list)) return result;
    for (std::size_t i = 0; i < list.size; ++i) {
        const auto& item = list.list[i];
        if (item.schema_id && item.name) result.emplace_back(item.schema_id, item.name);
    }
    return result;
}

bool RimeRuntime::validateSchemas(int& errorCode) {
    const auto schemas = schemaList();
    if (schemas.empty() || std::none_of(schemas.begin(), schemas.end(), [](const auto& item) { return item.first == "xmjd6"; })) {
        errorCode = kRuntimeErrorSchema;
        return false;
    }
    const RimeSessionId probe = api_->create_session();
    if (!probe) { errorCode = kRuntimeErrorSession; return false; }
    bool valid = true;
    for (const auto& schema : schemas) {
        if (!api_->select_schema(probe, schema.first.c_str()) || !sessionHealthy(probe, schema.first.c_str())) {
            valid = false;
            break;
        }
    }
    api_->destroy_session(probe);
    if (!valid) errorCode = kRuntimeErrorSchema;
    return valid;
}

std::filesystem::path RimeRuntime::settingsPath() const {
    return rootDirectory_ / L"settings.ini";
}

std::string RimeRuntime::readSettingsSchema() const {
    const auto contents = readSmallFile(settingsPath());
    if (!contents) return {};
    std::size_t offset = 0;
    while (offset < contents->size()) {
        const auto end = contents->find('\n', offset);
        const auto line = trimAscii(std::string_view(*contents).substr(
            offset, end == std::string::npos ? std::string::npos : end - offset));
        const auto equals = line.find('=');
        if (equals != std::string::npos && line.compare(0, equals, "schema") == 0) {
            const std::string value = trimAscii(std::string_view(line).substr(equals + 1));
            if (validSchemaId(value)) return value;
        }
        if (end == std::string::npos) break;
        offset = end + 1;
    }
    return {};
}

bool RimeRuntime::writeSettingsSchema(const std::string& schema) {
    if (!validSchemaId(schema)) return false;
    const auto temporary = settingsPath().wstring() + L".tmp";
    if (!writeFile(temporary, "schema=" + schema + "\n")) return false;
    return MoveFileExW(temporary.c_str(), settingsPath().c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

bool RimeRuntime::loadSelectedSchema() {
    const auto schemas = schemaList();
    if (schemas.empty()) return false;
    const std::string desired = readSettingsSchema();
    const auto requested = std::find_if(schemas.begin(), schemas.end(),
        [&](const auto& item) { return item.first == desired; });
    if (requested != schemas.end()) {
        selectedSchema_ = requested->first;
        return true;
    }
    const auto main = std::find_if(schemas.begin(), schemas.end(),
        [](const auto& item) { return item.first == "xmjd6"; });
    selectedSchema_ = main != schemas.end() ? main->first : schemas.front().first;
    return true;
}

void RimeRuntime::rememberSessionSchema(RimeSessionId session) {
    if (!api_ || !api_->get_status || !api_->free_status || session == 0) return;
    RimeStatus status{};
    RIME_STRUCT_INIT(RimeStatus, status);
    const std::unique_ptr<RimeStatus, decltype(api_->free_status)> cleanup(&status, api_->free_status);
    if (!api_->get_status(session, &status) || !status.schema_id) return;
    const std::string schema(status.schema_id);
    if (!validSchemaId(schema) || schema == selectedSchema_) return;
    selectedSchema_ = schema;
    writeSettingsSchema(schema);
}

bool RimeRuntime::sessionHealthy(RimeSessionId session, const char* expectedSchema) {
    RimeStatus status{};
    RIME_STRUCT_INIT(RimeStatus, status);
    const std::unique_ptr<RimeStatus, decltype(api_->free_status)> cleanup(&status, api_->free_status);
    if (!api_->get_status(session, &status)) return false;
    return !status.is_disabled && status.schema_id && status.schema_id[0] &&
           (!expectedSchema || std::string_view(status.schema_id) == expectedSchema);
}

bool RimeRuntime::redeploy(int& errorCode) {
    errorCode = 0;
    if (!initialized_ || !api_) {
        errorCode = kRuntimeErrorDeployment;
        return false;
    }
    ready_ = false;
    for (auto& entry : sessions_) {
        if (entry.second.id) api_->destroy_session(entry.second.id);
    }
    sessions_.clear();
    if (!runDeployment(errorCode)) return false;
    if (!validateSchemas(errorCode)) return false;
    if (!loadSelectedSchema()) {
        errorCode = kRuntimeErrorSchema;
        return false;
    }
    DeleteFileW((stagingDirectory_ / L".xmjd-deploy-pending").c_str());
    ready_ = true;
    return true;
}

bool RimeRuntime::ready() const { return ready_; }

std::uint64_t RimeRuntime::createSession() {
    if (!ready_ || !api_) return 0;
    const RimeSessionId id = api_->create_session();
    if (!id || selectedSchema_.empty() || !api_->select_schema(id, selectedSchema_.c_str()) ||
        !sessionHealthy(id, selectedSchema_.c_str())) {
        if (id) api_->destroy_session(id);
        return 0;
    }
    const std::uint64_t key = nextSession_++;
    if (key == 0 || nextSession_ == 0) {
        api_->destroy_session(id);
        return 0;
    }
    try {
        sessions_.emplace(key, Session{id, {}});
    } catch (...) {
        api_->destroy_session(id);
        throw;
    }
    return key;
}

RimeRuntime::Session* RimeRuntime::findSession(std::uint64_t session) {
    const auto found = sessions_.find(session);
    return found == sessions_.end() ? nullptr : &found->second;
}

void RimeRuntime::destroySession(std::uint64_t session) {
    const auto found = sessions_.find(session);
    if (found == sessions_.end()) return;
    if (found->second.id) {
        // F4 switcher changes only the live session. Persist that choice so a
        // new field or application keeps the user's scheme.
        rememberSessionSchema(found->second.id);
        api_->destroy_session(found->second.id);
    }
    sessions_.erase(found);
}

bool RimeRuntime::processKey(std::uint64_t session, std::uint32_t key, std::uint32_t modifiers) {
    auto* state = findSession(session);
    if (!ready_ || !state || key > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
        modifiers > static_cast<std::uint32_t>(std::numeric_limits<int>::max())) return false;
    if (!api_->find_session(state->id)) return false;
    const bool handled = api_->process_key(state->id, static_cast<int>(key), static_cast<int>(modifiers)) != 0;
    return handled && api_->find_session(state->id);
}

bool RimeRuntime::selectCandidate(std::uint64_t session, std::uint32_t pageIndex) {
    auto* state = findSession(session);
    if (!ready_ || !state || pageIndex >= kMaxCandidates || !api_->find_session(state->id)) return false;
    return api_->select_candidate_on_current_page(state->id, pageIndex) != 0;
}

void RimeRuntime::finalize(std::uint64_t session, bool convert) {
    auto* state = findSession(session);
    if (!ready_ || !state || !api_->find_session(state->id)) return;
    const char* input = api_->get_input(state->id);
    const std::string raw = input ? input : "";
    if (convert && api_->commit_composition(state->id)) {
        state->pendingRawCommit = takeCommit(api_, state->id);
    }
    if (state->pendingRawCommit.empty()) state->pendingRawCommit = raw;
    api_->clear_composition(state->id);
}

void RimeRuntime::cancel(std::uint64_t session) {
    auto* state = findSession(session);
    if (!ready_ || !state || !api_->find_session(state->id)) return;
    state->pendingRawCommit.clear();
    api_->clear_composition(state->id);
    (void)takeCommit(api_, state->id);
}

Snapshot RimeRuntime::snapshot(std::uint64_t session, bool drainCommit) {
    Snapshot result;
    auto* state = findSession(session);
    if (!ready_ || !state || !api_->find_session(state->id)) return result;
    if (drainCommit) {
        result.commitText = std::move(state->pendingRawCommit);
        state->pendingRawCommit.clear();
        result.commitText += takeCommit(api_, state->id);
    }
    if (const char* input = api_->get_input(state->id)) result.rawInput.assign(input);

    RimeContext context{};
    RIME_STRUCT_INIT(RimeContext, context);
    const std::unique_ptr<RimeContext, decltype(api_->free_context)> contextCleanup(&context, api_->free_context);
    if (api_->get_context(state->id, &context)) {
        if (context.composition.preedit) result.preedit.assign(context.composition.preedit);
        const int cursor = std::max(0, context.composition.cursor_pos);
        result.caretUtf8Offset = std::min<std::size_t>(static_cast<std::size_t>(cursor), result.preedit.size());
        result.page = context.menu.page_no > 0 ? static_cast<std::uint32_t>(context.menu.page_no) : 0;
        result.pageSize = context.menu.page_size > 0 ? static_cast<std::uint32_t>(context.menu.page_size) : 0;
        result.highlightedIndex = context.menu.highlighted_candidate_index > 0
            ? static_cast<std::uint32_t>(context.menu.highlighted_candidate_index) : 0;
        result.lastPage = context.menu.is_last_page != 0;
        const int candidateCount = std::clamp(context.menu.num_candidates, 0, static_cast<int>(kMaxCandidates));
        for (int i = 0; i < candidateCount; ++i) {
            const auto& candidate = context.menu.candidates[i];
            if (!candidate.text) continue;
            Candidate item;
            item.text.assign(candidate.text);
            if (candidate.comment) item.comment.assign(candidate.comment);
            item.label = selectionLabel(context, i);
            result.candidates.push_back(std::move(item));
        }
    }

    RimeStatus status{};
    RIME_STRUCT_INIT(RimeStatus, status);
    const std::unique_ptr<RimeStatus, decltype(api_->free_status)> statusCleanup(&status, api_->free_status);
    if (api_->get_status(state->id, &status)) {
        result.asciiMode = status.is_ascii_mode != 0;
    }
    return result;
}
} // namespace xmjd::win
