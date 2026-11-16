#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "../Common/CandidateStyle.h"

namespace xmjd::tip {

struct CandidateValue;
struct CandidateSnapshot;

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
    const xmjd::CandidateStyle& style() const { return style_; }

    // Lays out at most `maxRows` candidates for the given screen DPI.
    bool prepare(const std::vector<CandidateValue>& values, UINT selected, UINT maxRows, int dpi);
    // Paints the prepared layout into the layered window at (x, y).
    bool paint(HWND window, int dpi, int x, int y);
    // Repaints using the last content at the same position (DPI changes).
    bool repaint(HWND window, int dpi);

    // -1 when the point is outside every candidate.
    int hitTest(POINT client) const;
    const xmjd::CandidateLayout& layout() const { return layout_; }

private:
    bool paintInternal(HWND window, int dpi, int x, int y);
    int measureText(std::wstring_view text, int pixelSize);

    xmjd::CandidateStyle style_;
    xmjd::CandidateLayout layout_;
    std::vector<CandidateValue> values_;
    UINT selected_ = 0;
    int lastX_ = 0;
    int lastY_ = 0;
    int lastDpi_ = 0;
    std::uint64_t styleStamp_ = 0;
    std::wstring resolvedFace_;
};

} // namespace xmjd::tip
