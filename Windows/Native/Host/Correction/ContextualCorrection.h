#pragma once

#include <filesystem>
#include <future>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_set>

namespace xmjd::correction {

// Hooks only generated, private Rime files; the external scheme stays read-only.
std::unordered_set<std::string> installHooks(const std::filesystem::path& compiled,
    const std::filesystem::path& shared, const std::filesystem::path& user,
    const std::filesystem::path& resources);

bool validCode(std::string_view code) noexcept;
std::string trailingContext(std::string_view text);

class Worker {
public:
    explicit Worker(std::filesystem::path resources);
    ~Worker();
    std::future<std::string> submit(std::string context, std::string pool);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace xmjd::correction
