#include "StylePreview.h"

// WIN32_LEAN_AND_MEAN 会把 objidl 排除在 windows.h 之外；GDI+ 需要
// IStream / PROPID 的声明，所以先手动包含。
#include <objidl.h>
#include <gdiplus.h>

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <mutex>
#include <string>
#include <vector>

#include "../Common/CandidatePainter.h"

using namespace Gdiplus;
// 皮肤结构与排版辅助函数都在 xmjd 命名空间里，下面的匿名命名空间是
// 全局作用域的，必须显式引入，否则 ColorPair/StyleColor 无法解析。
using namespace xmjd;

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

std::wstring toWide(const std::string& utf8) {
    if (utf8.empty()) return {};
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                                          static_cast<int>(utf8.size()), nullptr, 0);
    if (count <= 0) return {};
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                            static_cast<int>(utf8.size()), result.data(), count) != count) return {};
    return result;
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
        const std::wstring wide = toWide(requested);
        if (faceInstalled(wide)) return wide;
    }
    if (faceInstalled(L"Microsoft YaHei UI")) return L"Microsoft YaHei UI";
    if (faceInstalled(L"Microsoft YaHei")) return L"Microsoft YaHei";
    return L"Segoe UI";
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

// 预览底板上只画一层圆角矩形，用来模拟桌面、让圆角和半透明有参照。
void fillBackdrop(Graphics& graphics, const Rect& rect, const Color& color) {
    GraphicsPath path;
    const int radius = std::min({8, rect.Width / 2, rect.Height / 2});
    if (radius <= 0) {
        path.AddRectangle(rect);
    } else {
        const int diameter = radius * 2;
        path.AddArc(rect.X, rect.Y, diameter, diameter, 180, 90);
        path.AddArc(rect.X + rect.Width - diameter, rect.Y, diameter, diameter, 270, 90);
        path.AddArc(rect.X + rect.Width - diameter, rect.Y + rect.Height - diameter, diameter, diameter, 0, 90);
        path.AddArc(rect.X, rect.Y + rect.Height - diameter, diameter, diameter, 90, 90);
        path.CloseFigure();
    }
    SolidBrush brush(color);
    graphics.FillPath(&brush, &path);
}
} // namespace

namespace xmjd::preview {
namespace {

void paintInto(Graphics& graphics, const RECT& bounds, const CandidateStyle& style, int dpi, bool dark) {
    const int width = bounds.right - bounds.left;
    const int height = bounds.bottom - bounds.top;
    const int scaleDpi = dpi <= 0 ? 96 : dpi;
    const std::wstring face = chooseFace(style.fontName);

    graphics.SetSmoothingMode(SmoothingModeAntiAlias);
    graphics.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    fillBackdrop(graphics, Rect(bounds.left, bounds.top, width, height),
                 dark ? Color(255, 40, 40, 44) : Color(255, 226, 226, 230));

    CandidateStyle shown = style;
    const double scale = static_cast<double>(scaleDpi) / 96.0;
    const int margin = candidateShadowMargin(scaleDpi);
    const int available = std::max(120, width - 24 - margin * 2);
    shown.maxWidth = std::min(shown.maxWidth, available / scale);

    std::vector<CandidateItem> items = {
        {L"1", L"可以", L"ke yi"},
        {L"2", L"刻意", L"ke yi"},
        {L"3", L"可以", L""},
        {L"4", L"可是", L"ke shi"},
        {L"5", L"可疑", L"ke yi"},
    };
    const CandidateTextMeasure measure = [&](std::wstring_view text, int size) {
        return measureText(face, text, size);
    };

    CandidateLayout layout = layoutCandidates(shown, items, scaleDpi, measure);
    // 竖排或大字号下 5 条可能放不下，逐条减少到能装进预览框（含投影留白）。
    while (items.size() > 2 &&
           (layout.width + margin * 2 > width - 8 || layout.height + margin * 2 > height - 8)) {
        items.pop_back();
        layout = layoutCandidates(shown, items, scaleDpi, measure);
    }
    if (layout.width <= 0 || layout.height <= 0) return;

    const int x = bounds.left + (width - layout.width) / 2;
    const int y = bounds.top + (height - layout.height) / 2;
    // 高亮第 0 条：和刚上屏、还没选过时的候选窗一致。
    paintCandidateSurface(graphics, POINT{x - margin, y - margin}, face, shown, items, layout, 0,
                          scaleDpi, dark);
}

} // namespace

void paint(HDC target, const RECT& bounds, const CandidateStyle& style, int dpi, bool dark) {
    ensureGdiplus();
    if (!target) return;
    if (bounds.right - bounds.left <= 16 || bounds.bottom - bounds.top <= 16) return;
    Graphics graphics(target);
    paintInto(graphics, bounds, style, dpi, dark);
}

void paint(Graphics& target, const RECT& bounds, const CandidateStyle& style, int dpi, bool dark) {
    ensureGdiplus();
    if (bounds.right - bounds.left <= 16 || bounds.bottom - bounds.top <= 16) return;
    paintInto(target, bounds, style, dpi, dark);
}

} // namespace xmjd::preview
