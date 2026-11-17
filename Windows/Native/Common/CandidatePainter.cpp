#include "CandidatePainter.h"

// WIN32_LEAN_AND_MEAN 会把 objidl 排除在 windows.h 之外；GDI+ 需要
// IStream / PROPID 的声明，所以先手动包含。
#include <objidl.h>
#include <gdiplus.h>

#include <algorithm>
#include <cmath>

using namespace Gdiplus;

namespace xmjd {
namespace {

HDC g_measureDc = nullptr;

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

// 由外向内叠若干层低透明度圆角矩形，合成一条柔和的投影。层数少、单层很淡，
// 叠出来的边缘在浅色和深色底上都看不出台阶。
void paintShadow(Graphics& graphics, const Rect& panel, int radius, int margin, bool dark) {
    if (margin <= 0) return;
    const double peak = dark ? 76.0 : 34.0;
    for (int step = margin; step >= 1; --step) {
        const double fade = 1.0 - static_cast<double>(step) / (margin + 1.0);
        const int alpha = static_cast<int>(std::lround(peak * fade * fade * 0.5));
        if (alpha <= 0) continue;
        // 只向下偏移 1px，模拟面板是贴在光标下方、光源在上方的感觉。
        const Rect ring(panel.X - step, panel.Y - step + 1, panel.Width + step * 2, panel.Height + step * 2);
        GraphicsPath path;
        buildRoundedPath(path, ring, radius + step);
        SolidBrush brush(Color(static_cast<BYTE>(alpha), 0, 0, 0));
        graphics.FillPath(&brush, &path);
    }
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

int measureText(const std::wstring& face, std::wstring_view text, int pixelSize) {
    if (text.empty()) return 0;
    if (!g_measureDc) g_measureDc = CreateCompatibleDC(nullptr);
    if (!g_measureDc) return static_cast<int>(text.size()) * pixelSize / 2;
    Graphics graphics(g_measureDc);
    FontFamily family(face.c_str());
    if (family.GetLastStatus() != Ok) return static_cast<int>(text.size()) * pixelSize / 2;
    Font font(&family, static_cast<REAL>(pixelSize), FontStyleRegular, UnitPixel);
    if (font.GetLastStatus() != Ok) return static_cast<int>(text.size()) * pixelSize / 2;
    StringFormat format(StringFormat::GenericTypographic());
    format.SetFormatFlags(StringFormatFlagsNoWrap | StringFormatFlagsMeasureTrailingSpaces);
    RectF bounds;
    graphics.MeasureString(text.data(), static_cast<INT>(text.size()), &font, PointF(0, 0), &format, &bounds);
    return static_cast<int>(std::ceil(bounds.Width));
}
} // namespace

int candidateShadowMargin(int dpi) {
    return std::max(4, stylePixels(8.0, dpi <= 0 ? 96 : dpi));
}

void paintCandidateSurface(Graphics& graphics, POINT origin, const std::wstring& face,
                           const CandidateStyle& style, const std::vector<CandidateItem>& items,
                           const CandidateLayout& layout, unsigned selected, int dpi, bool dark) {
    if (layout.width <= 0 || layout.height <= 0 || items.empty()) return;
    const int margin = candidateShadowMargin(dpi);
    const Rect panel(origin.x + margin, origin.y + margin, layout.width, layout.height);
    const int radius = stylePixels(style.cornerRadius, dpi);
    const int itemRadius = stylePixels(style.itemRadius, dpi);
    const CandidateMetrics metrics = candidateMetrics(style, dpi);
    const double opacity = style.opacity;

    paintShadow(graphics, panel, radius, margin, dark);

    const Color background = pairColor(style.background, dark, opacity);
    const Color backgroundEnd = style.decoration
        ? pairColor(style.decoration->backgroundEnd, dark, opacity)
        : background;
    fillRounded(graphics, panel, radius, background, backgroundEnd);

    const Color textColor = pairColor(style.text, dark, opacity);
    const Color selectedText = style.decoration
        ? pairColor(style.decoration->selectedText, dark, opacity)
        : Color(255, 255, 255, 255);
    const Color secondary = style.decoration
        ? pairColor(style.decoration->secondaryText, dark, 1.0)
        : pairColor(style.text, dark, 0.7);
    const unsigned highlighted = items.empty() ? 0 : std::min<unsigned>(selected, static_cast<unsigned>(items.size() - 1));

    for (std::size_t index = 0; index < layout.items.size() && index < items.size(); ++index) {
        const CandidateBox& box = layout.items[index];
        const bool isSelected = index == highlighted;
        if (isSelected) {
            const Color start = style.decoration
                ? pairColor(style.decoration->highlightMiddle, dark, opacity)
                : pairColor(style.accent, dark, opacity);
            const Color end = style.decoration
                ? pairColor(style.decoration->highlightEnd, dark, opacity)
                : start;
            fillRounded(graphics, Rect(panel.X + box.x, panel.Y + box.y, box.width, box.height),
                        itemRadius, start, end);
        }
        REAL cursor = static_cast<REAL>(panel.X + box.x + metrics.padX);
        const REAL baseline = static_cast<REAL>(panel.Y + box.y);
        const REAL rowHeight = static_cast<REAL>(box.height);
        if (style.showLabels && !items[index].label.empty()) {
            const int labelWidth = measureText(face, items[index].label, metrics.labelSize);
            drawText(graphics, items[index].label, face, metrics.labelSize,
                     isSelected ? selectedText : secondary, cursor, baseline,
                     static_cast<REAL>(labelWidth), rowHeight);
            cursor += labelWidth + metrics.labelGap;
        }
        const int textWidth = measureText(face, items[index].text, metrics.fontSize);
        drawText(graphics, items[index].text, face, metrics.fontSize,
                 isSelected ? selectedText : textColor, cursor, baseline,
                 static_cast<REAL>(textWidth), rowHeight);
        cursor += textWidth;
        if (style.showComments && !items[index].comment.empty()) {
            cursor += metrics.commentGap;
            drawText(graphics, items[index].comment, face, metrics.labelSize, secondary, cursor, baseline,
                     static_cast<REAL>(panel.X + layout.width - cursor - metrics.padding), rowHeight);
        }
    }
}

} // namespace xmjd
