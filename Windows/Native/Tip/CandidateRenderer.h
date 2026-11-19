#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "../Common/CandidateStyle.h"

namespace xmjd::tip {

struct CandidateValue;
struct CandidateSnapshot;
class CandidateComposition;

// Paints the candidate popup from the shared "xmjd.candidate-style" document.
// The same JSON is what the Mac build edits, so skins travel between platforms.
class CandidateRenderer {
public:
    CandidateRenderer();
    ~CandidateRenderer();
    CandidateRenderer(const CandidateRenderer&) = delete;
    CandidateRenderer& operator=(const CandidateRenderer&) = delete;

    // Re-reads CandidateStyles\active.json when the file changed.
    void reloadIfNeeded();
    void setAppearance(xmjd::CandidateStyle style, bool dark);
    const xmjd::CandidateStyle& style() const { return style_; }

    // Lays out at most `maxRows` candidates for the given screen DPI.
    bool prepare(const std::vector<CandidateValue>& values, UINT selected, UINT maxRows, int dpi, bool withShadow = true);
    // Paints the prepared layout into the layered window at (x, y).
    bool paint(HWND window, int dpi, int x, int y);
    HRESULT paintComposited(HWND window);
    void resetComposition() noexcept;
    bool paintOpaque(HDC dc, const RECT& client);
    // Repaints using the last content at the same position (DPI changes).
    bool repaint(HWND window, int dpi);

    // -1 when the point is outside every candidate.
    int hitTest(POINT client) const;
    const xmjd::CandidateLayout& layout() const { return layout_; }
    // 面板外面还包着一圈投影，窗口比 layout 大这么多；定位和命中测试都要按它换算。
    int shadowMargin() const { return shadowMargin_; }

private:
    bool paintInternal(HWND window, int dpi, int x, int y);
    bool renderPixels(void* bits, int width, int height, int dpi);
    int measureText(std::wstring_view text, int pixelSize);

    xmjd::CandidateStyle style_;
    xmjd::CandidateLayout layout_;
    std::vector<CandidateItem> items_;
    std::vector<CandidateValue> values_;
    UINT selected_ = 0;
    int shadowMargin_ = 0;
    int lastX_ = 0;
    int lastY_ = 0;
    int lastDpi_ = 0;
    std::uint64_t styleStamp_ = 0;
    std::wstring resolvedFace_;
    std::optional<bool> hostDark_;
    std::unique_ptr<CandidateComposition> composition_;
    std::vector<std::uint32_t> pixels_;
};

} // namespace xmjd::tip
