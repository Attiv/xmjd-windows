#include "CandidateRenderer.h"
#include "CandidateComposition.h"

// WIN32_LEAN_AND_MEAN keeps objidl out of windows.h; GDI+ needs IStream and
// PROPID declarations before gdiplus.h is parsed.
#include <objidl.h>
#include <gdiplus.h>

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <mutex>

#include "Tip.h"
#include "../Common/CandidatePainter.h"

using namespace Gdiplus;

namespace xmjd::tip {
namespace {

ULONG_PTR g_gdiplusToken = 0;
std::once_flag g_gdiplusOnce;
HDC g_measureDc = nullptr;

void ensureGdiplus() {
    std::call_once(g_gdiplusOnce, [] {
        GdiplusStartupInput input;
        GdiplusStartup(&g_gdiplusToken, &input, nullptr);
    });
}

int CALLBACK countFace(const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM parameter) {
    *reinterpret_cast<int*>(parameter) += 1;
    return 0;
}

bool faceInstalled(const std::wstring& face) {
    if (face.empty()) return false;
    HDC dc = CreateCompatibleDC(nullptr);
    if (!dc) return false;
    LOGFONTW format{};
    format.lfCharSet = DEFAULT_CHARSET;
    wcsncpy_s(format.lfFaceName, face.c_str(), _TRUNCATE);
    int count = 0;
    EnumFontFamiliesExW(dc, &format, countFace, reinterpret_cast<LPARAM>(&count), 0);
    DeleteDC(dc);
    return count > 0;
}

std::wstring chooseFace(const std::string& requested) {
    if (!requested.empty()) {
        const std::wstring wide = xmjd::win::widen(requested);
        if (faceInstalled(wide)) return wide;
    }
    if (faceInstalled(L"Microsoft YaHei UI")) return L"Microsoft YaHei UI";
    if (faceInstalled(L"Microsoft YaHei")) return L"Microsoft YaHei";
    return L"Segoe UI";
}

std::uint64_t styleStamp(const std::filesystem::path& path) {
    std::error_code ec;
    const auto time = std::filesystem::last_write_time(path, ec);
    if (ec) return 0;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) return 0;
    return static_cast<std::uint64_t>(time.time_since_epoch().count()) * 131 +
           static_cast<std::uint64_t>(size);
}

} // namespace

CandidateRenderer::CandidateRenderer() {
    ensureGdiplus();
    reloadIfNeeded();
}

CandidateRenderer::~CandidateRenderer() = default;

void CandidateRenderer::reloadIfNeeded() {
    ensureGdiplus();
    if (hostDark_.has_value()) return;
    const auto dataRoot = xmjd::win::dataRoot();
    const auto path = xmjd::activeCandidateStylePath(dataRoot);
    const std::uint64_t stamp = styleStamp(path);
    if (style_.id.empty() || stamp != styleStamp_) {
        style_ = xmjd::loadActiveCandidateStyle(dataRoot);
        styleStamp_ = stamp;
        resolvedFace_ = chooseFace(style_.fontName);
    }
}

void CandidateRenderer::setAppearance(xmjd::CandidateStyle style, bool dark) {
    auto face = chooseFace(style.fontName);
    style_ = std::move(style);
    resolvedFace_ = std::move(face);
    hostDark_ = dark;
}

int CandidateRenderer::measureText(std::wstring_view text, int pixelSize) {
    if (text.empty()) return 0;
    if (!g_measureDc) g_measureDc = CreateCompatibleDC(nullptr);
    if (!g_measureDc) return static_cast<int>(text.size()) * pixelSize / 2;
    Graphics graphics(g_measureDc);
    FontFamily family(resolvedFace_.empty() ? L"Segoe UI" : resolvedFace_.c_str());
    if (family.GetLastStatus() != Ok) return static_cast<int>(text.size()) * pixelSize / 2;
    Font font(&family, static_cast<REAL>(pixelSize), FontStyleRegular, UnitPixel);
    if (font.GetLastStatus() != Ok) return static_cast<int>(text.size()) * pixelSize / 2;
    StringFormat format(StringFormat::GenericTypographic());
    format.SetFormatFlags(StringFormatFlagsNoWrap | StringFormatFlagsMeasureTrailingSpaces);
    RectF bounds;
    graphics.MeasureString(text.data(), static_cast<INT>(text.size()), &font, PointF(0, 0), &format, &bounds);
    return static_cast<int>(std::ceil(bounds.Width));
}

bool CandidateRenderer::prepare(const std::vector<CandidateValue>& values, UINT selected, UINT maxRows, int dpi, bool withShadow) {
    if (values.empty()) return false;
    reloadIfNeeded();
    const std::size_t count = std::min<std::size_t>(values.size(), std::max<UINT>(1, maxRows));
    items_.clear();
    items_.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        items_.push_back(CandidateItem{values[index].label, values[index].text, values[index].comment});
    }
    layout_ = xmjd::layoutCandidates(style_, items_, dpi,
                                    [this](std::wstring_view text, int size) { return measureText(text, size); });
    shadowMargin_ = withShadow ? xmjd::candidateShadowMargin(dpi) : 0;
    lastDpi_ = dpi;
    values_.assign(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(count));
    selected_ = std::min<UINT>(selected, static_cast<UINT>(count - 1));
    return layout_.width > 0 && layout_.height > 0;
}

bool CandidateRenderer::paint(HWND window, int dpi, int x, int y) {
    if (!window || layout_.width <= 0) return false;
    lastX_ = x;
    lastY_ = y;
    lastDpi_ = dpi;
    return paintInternal(window, dpi, x, y);
}

bool CandidateRenderer::paintOpaque(HDC dc, const RECT& client) {
    const bool dark = hostDark_ ? *hostDark_ : xmjd::windowsCandidateDarkMode();
    if (g_gdiplusToken && FillRect(dc, &client, GetSysColorBrush(COLOR_WINDOW))) {
        Graphics graphics(dc);
        graphics.SetSmoothingMode(SmoothingModeAntiAlias);
        graphics.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
        if (xmjd::paintCandidateSurface(graphics, POINT{0, 0}, resolvedFace_, style_, items_, layout_,
                                        selected_, lastDpi_, dark, false)) return true;
    }
    return xmjd::paintOpaqueCandidateSurface(dc, client, resolvedFace_, style_, items_, layout_, selected_, lastDpi_);
}

HRESULT CandidateRenderer::paintComposited(HWND window) {
    const int width = layout_.width + shadowMargin_ * 2;
    const int height = layout_.height + shadowMargin_ * 2;
    if (width <= 0 || height <= 0 || width > 8192 || height > 8192 ||
        static_cast<std::size_t>(width) * height > 16 * 1024 * 1024) return E_INVALIDARG;
    if (!composition_) composition_ = std::make_unique<CandidateComposition>();
    pixels_.resize(static_cast<std::size_t>(width) * height);
    if (!renderPixels(pixels_.data(), width, height, lastDpi_)) return E_FAIL;
    return composition_->present(window, width, height, pixels_.data());
}

void CandidateRenderer::resetComposition() noexcept {
    composition_.reset();
    std::vector<std::uint32_t>().swap(pixels_);
}

bool CandidateRenderer::repaint(HWND window, int dpi) {
    if (!window || layout_.width <= 0) return false;
    lastDpi_ = dpi;
    return paintInternal(window, dpi, lastX_, lastY_);
}

bool CandidateRenderer::paintInternal(HWND window, int dpi, int x, int y) {
    if (layout_.width <= 0 || layout_.height <= 0 || items_.empty()) return false;
    const int margin = shadowMargin_ > 0 ? shadowMargin_ : xmjd::candidateShadowMargin(dpi);
    // 窗口比面板大一圈：投影画在面板外，必须有自己的像素位置。
    const int width = layout_.width + margin * 2;
    const int height = layout_.height + margin * 2;
    // Use the default palette instead of requiring a desktop-wide screen DC.
    HDC memory = CreateCompatibleDC(nullptr);
    if (!memory) return false;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(memory, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bitmap || !bits) {
        const DWORD error = GetLastError();
        if (bitmap) DeleteObject(bitmap);
        DeleteDC(memory);
        SetLastError(error);
        return false;
    }
    HGDIOBJ previous = SelectObject(memory, bitmap);
    if (!previous || previous == HGDI_ERROR) {
        const DWORD error = GetLastError();
        DeleteObject(bitmap);
        DeleteDC(memory);
        SetLastError(error);
        return false;
    }
    if (!renderPixels(bits, width, height, dpi)) {
        SelectObject(memory, previous);
        DeleteObject(bitmap);
        DeleteDC(memory);
        SetLastError(ERROR_INVALID_DATA);
        return false;
    }

    BLENDFUNCTION blend{};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;
    POINT destination{x - margin, y - margin};
    SIZE size{width, height};
    POINT source{0, 0};
    // 位图必须仍在 memory DC 里，UpdateLayeredWindow 才能读到图内容。
    const BOOL painted = UpdateLayeredWindow(window, nullptr, &destination, &size, memory, &source, 0, &blend, ULW_ALPHA);
    const DWORD error = painted ? ERROR_SUCCESS : GetLastError();
    SelectObject(memory, previous);
    DeleteObject(bitmap);
    DeleteDC(memory);
    SetLastError(error);
    return painted != FALSE;
}

bool CandidateRenderer::renderPixels(void* bits, int width, int height, int dpi) {
    // Both presenters need premultiplied alpha, which GDI+ won't write through an HDC.
    Bitmap surface(width, height, width * 4, PixelFormat32bppPARGB, static_cast<BYTE*>(bits));
    Graphics graphics(&surface);
    if (surface.GetLastStatus() != Ok || graphics.GetLastStatus() != Ok) return false;
    graphics.SetCompositingMode(CompositingModeSourceCopy);
    graphics.Clear(Color(0, 0, 0, 0));
    graphics.SetCompositingMode(CompositingModeSourceOver);
    graphics.SetSmoothingMode(SmoothingModeAntiAlias);
    graphics.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    return xmjd::paintCandidateSurface(graphics, POINT{0, 0}, resolvedFace_, style_, items_, layout_,
                                       selected_, dpi, hostDark_ ? *hostDark_ : xmjd::windowsCandidateDarkMode());
}

int CandidateRenderer::hitTest(POINT client) const {
    // 客户区含投影留白，候选盒坐标是面板内的，先换算回面板坐标。
    const int margin = shadowMargin_;
    const POINT point{client.x - margin, client.y - margin};
    for (std::size_t index = 0; index < layout_.items.size(); ++index) {
        const CandidateBox& box = layout_.items[index];
        if (point.x >= box.x && point.x < box.x + box.width && point.y >= box.y &&
            point.y < box.y + box.height) {
            return static_cast<int>(index);
        }
    }
    return -1;
}

} // namespace xmjd::tip
