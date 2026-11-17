#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "../Common/CandidateStyle.h"

// 只前向声明，避免把 gdiplus.h 带进每个包含者。
namespace Gdiplus {
class Graphics;
}

namespace xmjd::preview {

// 在设置页里用与候选窗相同的排版/配色画一张预览。
// `bounds` 是目标控件的客户区坐标；`dark` 只影响取 light 还是 dark 配色。
void paint(HDC target, const RECT& bounds, const CandidateStyle& style, int dpi, bool dark);

// 同一个绘制流程的离屏版本：直接画进 GDI+ 目标（PNG 导出用，alpha 正确）。
void paint(Gdiplus::Graphics& target, const RECT& bounds, const CandidateStyle& style, int dpi, bool dark);

} // namespace xmjd::preview
