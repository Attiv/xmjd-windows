#include "CandidateRenderer.h"

#include <gdiplus.h>

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <mutex>

#include "Tip.h"

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
    HDC dc = GetDC(nullptr);
    if (!dc) return false;
    LOGFONTW format{};
    format.lfCharSet = DEFAULT_CHARSET;
    wcsncpy_s(format.lfFaceName, face.c_str(), _TRUNCATE);
    int count = 0;
    EnumFontFamiliesExW(dc, &format, countFace, reinterpret_cast<LPARAM>(&count), 0);
    ReleaseDC(nullptr, dc);
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

bool systemPrefersDark() {
    DWORD light = 1;
    DWORD size = sizeof(light);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size) != ERROR_SUCCESS) {
        return false;
    }
    return light == 0;
}

// Converts a resolved pair; `dark` selects the variant.
Color pairColor(const ColorPair& pair, bool dark, double opacity) {
    const StyleColor& value = dark ? pair.dark : pair.light;
    const double alpha = std::min(1.0, std::max(0.0, value.alpha * opacity));
    return Color(static_cast<BYTE>(std::lround(alpha * 255)),
                 static_cast<BYTE>(std::lround(std::clamp(value.red, 0.0, 1.0) * 255)),
                 static_cast<BYTE>(std::lround(std::clamp(value.green, 0.0, 1.0) * 255)),
                 static_cast<BYTE>(std::lround(std::clamp(value.blue, 0.0, 1.0) * 255)));
}

void buildRoundedPath(GraphicsPath& path, const Rect& rect, int radius) {
    const int limit = std::min({radius, rect.Width / 2, rect.Height / 2});
    if (limit <= 0) {
        path.AddRectangle(rect);
        return;
    }
    const int diameter = limit * 2;
    path.AddArc(rect.X, rect.Y, diameter, diameter, 180, 90);
    path.AddArc(rect.X + rect.Width - diameter, rect.Y, diameter, diameter, 270, 90);
    path.AddArc(rect.X + rect.Width - diameter, rect.Y + rect.Height - diameter, diameter, diameter, 0, 90);
    path.AddArc(rect.X, rect.Y + rect.Height - diameter, diameter, diameter, 90, 90);
    path.CloseFigure();
}

void fillRounded(Graphics& graphics, const Rect& rect, int radius, const Color& start, const Color& end) {
    GraphicsPath path;
    buildRoundedPath(path, rect, radius);
    if (start.GetValue() == end.GetValue()) {
        SolidBrush brush(start);
        graphics.FillPath(&brush, &path);
        return;
    }
    LinearGradientBrush brush(Point(rect.X, rect.Y), Point(rect.X + rect.Width, rect.Y + rect.Height), start, end);
    graphics.FillPath(&brush, &path);
}

void drawText(Graphics& graphics, const std::wstring& text, const std::wstring& face, int pixelSize,
              const Color& color, REAL x, REAL y, REAL width, REAL height) {
    if (text.empty()) return;
    FontFamily family(face.c_str());
    if (family.GetLastStatus() != Ok) return;
    Font font(&family, static_cast<REAL>(pixelSize), FontStyleRegular, UnitPixel);
    if (font.GetLastStatus() != Ok) return;
    SolidBrush brush(color);
    StringFormat format(StringFormat::GenericTypographic());
    format.SetFormatFlags(StringFormatFlagsNoWrap | StringFormatFlagsMeasureTrailingSpaces);
    format.SetLineAlignment(StringAlignmentCenter);
    format.SetTrimming(StringTrimmingEllipsisCharacter);
    RectF bounds(x, y, std::max<REAL>(1, width), height);
    graphics.DrawString(text.c_str(), static_cast<INT>(text.size()), &font, bounds, &format, &brush);
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
    const auto dataRoot = xmjd::win::dataRoot();
    const auto path = xmjd::activeCandidateStylePath(dataRoot);
    const std::uint64_t stamp = styleStamp(path);
    if (style_.id.empty() || stamp != styleStamp_) {
        style_ = xmjd::loadActiveCandidateStyle(dataRoot);
        styleStamp_ = stamp;
        resolvedFace_ = chooseFace(style_.fontName);
    }
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

bool CandidateRenderer::prepare(const std::vector<CandidateValue>& values, UINT selected, UINT maxRows, int dpi) {
    if (values.empty()) return false;
    reloadIfNeeded();
    const std::size_t count = std::min<std::size_t>(values.size(), std::max<UINT>(1, maxRows));
    std::vector<CandidateItem> items;
    items.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        items.push_back(CandidateItem{values[index].label, values[index].text, values[index].comment});
    }
    layout_ = xmjd::layoutCandidates(style_, items, dpi,
                                    [this](std::wstring_view text, int size) { return measureText(text, size); });
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

bool CandidateRenderer::repaint(HWND window, int dpi) {
    if (!window || layout_.width <= 0) return false;
    lastDpi_ = dpi;
    return paintInternal(window, dpi, lastX_, lastY_);
}

bool CandidateRenderer::paintInternal(HWND window, int dpi, int x, int y) {
    if (layout_.width <= 0 || layout_.height <= 0) return false;
    const int width = layout_.width;
    const int height = layout_.height;
    HDC screen = GetDC(nullptr);
    if (!screen) return false;
    HDC memory = CreateCompatibleDC(screen);
    if (!memory) {
        ReleaseDC(nullptr, screen);
        return false;
    }
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
        DeleteDC(memory);
        ReleaseDC(nullptr, screen);
        return false;
    }
    HGDIOBJ previous = SelectObject(memory, bitmap);
    const bool dark = systemPrefersDark();
    const CandidateMetrics metrics = xmjd::candidateMetrics(style_, dpi);
    const double opacity = style_.opacity;

    {
        Graphics graphics(memory);
        graphics.SetSmoothingMode(SmoothingModeAntiAlias);
        graphics.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
        graphics.SetCompositingMode(CompositingModeSourceOver);
        const Color background = pairColor(style_.background, dark, opacity);
        const Color backgroundEnd = style_.decoration
            ? pairColor(style_.decoration->backgroundEnd, dark, opacity)
            : background;
        const int radius = xmjd::stylePixels(style_.cornerRadius, dpi);
        fillRounded(graphics, Rect(0, 0, width, height), radius, background, backgroundEnd);

        const int itemRadius = xmjd::stylePixels(style_.itemRadius, dpi);
        const Color textColor = pairColor(style_.text, dark, opacity);
        const Color selectedText = style_.decoration
            ? pairColor(style_.decoration->selectedText, dark, opacity)
            : Color(255, 255, 255, 255);
        const Color secondary = style_.decoration
            ? pairColor(style_.decoration->secondaryText, dark, 1.0)
            : pairColor(style_.text, dark, 0.7);

        for (std::size_t index = 0; index < layout_.items.size(); ++index) {
            const CandidateBox& box = layout_.items[index];
            const bool isSelected = index == selected_;
            if (isSelected) {
                const Color start = style_.decoration
                    ? pairColor(style_.decoration->highlightMiddle, dark, opacity)
                    : pairColor(style_.accent, dark, opacity);
                const Color end = style_.decoration
                    ? pairColor(style_.decoration->highlightEnd, dark, opacity)
                    : start;
                fillRounded(graphics, Rect(box.x, box.y, box.width, box.height), itemRadius, start, end);
            }
            REAL cursor = static_cast<REAL>(box.x + metrics.padX);
            const REAL baseline = static_cast<REAL>(box.y);
            const REAL rowHeight = static_cast<REAL>(box.height);
            if (style_.showLabels && !values_[index].label.empty()) {
                const int labelWidth = measureText(values_[index].label, metrics.labelSize);
                drawText(graphics, values_[index].label, resolvedFace_, metrics.labelSize,
                         isSelected ? selectedText : secondary, cursor, baseline,
                         static_cast<REAL>(labelWidth), rowHeight);
                cursor += labelWidth + metrics.labelGap;
            }
            const int textWidth = measureText(values_[index].text, metrics.fontSize);
            drawText(graphics, values_[index].text, resolvedFace_, metrics.fontSize,
                     isSelected ? selectedText : textColor, cursor, baseline, static_cast<REAL>(textWidth),
                     rowHeight);
            cursor += textWidth;
            if (style_.showComments && !values_[index].comment.empty()) {
                cursor += metrics.commentGap;
                drawText(graphics, values_[index].comment, resolvedFace_, metrics.labelSize, secondary, cursor,
                         baseline, static_cast<REAL>(width - cursor - metrics.padding), rowHeight);
            }
        }
    }

    SelectObject(memory, previous);
    BLENDFUNCTION blend{};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;
    POINT destination{x, y};
    SIZE size{width, height};
    POINT source{0, 0};
    const BOOL painted = UpdateLayeredWindow(window, screen, &destination, &size, memory, &source, 0, &blend, ULW_ALPHA);
    DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);
    return painted != FALSE;
}

int CandidateRenderer::hitTest(POINT client) const {
    for (std::size_t index = 0; index < layout_.items.size(); ++index) {
        const CandidateBox& box = layout_.items[index];
        if (client.x >= box.x && client.x < box.x + box.width && client.y >= box.y &&
            client.y < box.y + box.height) {
            return static_cast<int>(index);
        }
    }
    return -1;
}

} // namespace xmjd::tip
