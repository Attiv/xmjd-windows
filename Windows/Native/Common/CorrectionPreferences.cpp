#include "CorrectionPreferences.h"
#include <windows.h>
#include <algorithm>
#include <fstream>
#include <array>
#include <charconv>
#include <string>

namespace xmjd {
namespace {
constexpr wchar_t kFile[] = L"correction.ini";

std::uint64_t parse(std::string_view text, std::uint64_t fallback) {
    std::uint64_t n = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), n);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size() ? n : fallback;
}
}

CorrectionPreferences loadCorrectionPreferences(const std::filesystem::path& dataRoot) {
    CorrectionPreferences out;
    if (!dataRoot.is_absolute()) return out;
    std::ifstream file(dataRoot / kFile, std::ios::binary);
    std::array<char, 513> buffer{};
    file.read(buffer.data(), buffer.size());
    const auto size = file.gcount();
    if (size <= 0 || size >= static_cast<std::streamsize>(buffer.size())) return out;
    std::string_view content(buffer.data(), static_cast<std::size_t>(size));
    while (!content.empty()) {
        const auto next = content.find('\n');
        auto line = content.substr(0, next);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        const auto separator = line.find('=');
        if (separator != line.npos) {
            const auto key = line.substr(0, separator), value = line.substr(separator + 1);
            if (key == "enabled") out.enabled = parse(value, 1) != 0;
            else if (key == "strength") out.strength = static_cast<int>(std::min<std::uint64_t>(100, parse(value, 35)));
            else if (key == "lead_tenths") out.leadTenths = static_cast<int>(std::clamp<std::uint64_t>(parse(value, 14), 10, 30));
            else if (key == "learning_reset") out.learningReset = parse(value, 0);
        }
        if (next == content.npos) break;
        content.remove_prefix(next + 1);
    }
    return out;
}

bool saveCorrectionPreferences(const std::filesystem::path& dataRoot, CorrectionPreferences p) {
    if (!dataRoot.is_absolute()) return false;
    std::error_code error;
    std::filesystem::create_directories(dataRoot, error);
    if (error) return false;
    const auto target = dataRoot / kFile;
    auto temporary = target;
    temporary += L"." + std::to_wstring(GetCurrentProcessId()) + L".tmp";
    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
    file << "enabled=" << p.enabled << "\nstrength=" << std::clamp(p.strength, 0, 100)
         << "\nlead_tenths=" << std::clamp(p.leadTenths, 10, 30)
         << "\nlearning_reset=" << p.learningReset << '\n';
    file.close();
    if (!file || !MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::filesystem::remove(temporary, error);
        return false;
    }
    return true;
}
} // namespace xmjd
