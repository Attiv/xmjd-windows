#pragma once

#include <cstdint>
#include <filesystem>

namespace xmjd {
struct CorrectionPreferences {
    bool enabled = true;
    bool singleCharacter = false;
    int strength = 35;
    int leadTenths = 14;
    std::uint64_t learningReset = 0;
};

CorrectionPreferences loadCorrectionPreferences(const std::filesystem::path& dataRoot);
bool saveCorrectionPreferences(const std::filesystem::path& dataRoot, CorrectionPreferences preferences);
} // namespace xmjd
