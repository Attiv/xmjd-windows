#pragma once
#include <cstdint>

namespace xmjd::tip {
class ShiftTap {
public:
    bool pending() const noexcept { return key_ != 0; }
    void reset() noexcept { key_ = 0; }

    // TSF can test a chord without forwarding the actual key down to the TIP.
    void observeKeyDown(std::uint32_t shiftKey) noexcept {
        if (key_ != shiftKey) reset();
    }

    void press(std::uint32_t key, std::uint64_t time, bool repeated, bool chord) noexcept {
        if (chord) reset();
        else if (!repeated && !pending()) {
            key_ = key;
            time_ = time;
        }
    }

    bool release(std::uint32_t key, std::uint64_t time, bool chord) noexcept {
        const bool tapped = pending() && key_ == key && !chord && time - time_ < 500;
        reset();
        return tapped;
    }

private:
    std::uint32_t key_ = 0;
    std::uint64_t time_ = 0;
};
} // namespace xmjd::tip
