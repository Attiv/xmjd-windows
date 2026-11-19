#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <string>
#include <vector>

#include "CandidateStyle.h"

// 只前向声明，避免 gdiplus.h 传染到调用方。
namespace Gdiplus {
class Graphics;
}

namespace xmjd {

// 面板外围留给投影的像素数。窗口尺寸必须等于 layout 尺寸加两倍这个值，
// 否则阴影会被窗口裁掉。
int candidateShadowMargin(int dpi);
bool windowsCandidateDarkMode() noexcept;

// 画整块候选面板：柔和投影 + 圆角底色 + 各条候选。`origin` 是窗口左上角，
// 面板本身画在 origin + (margin, margin) 处。
// 候选窗与设置页预览共用这一份实现，保证"预览里看到的就是实际弹出的样子"。
bool paintCandidateSurface(Gdiplus::Graphics& graphics, POINT origin, const std::wstring& face,
                          const CandidateStyle& style, const std::vector<CandidateItem>& items,
                          const CandidateLayout& layout, unsigned selected, int dpi, bool dark,
                          bool withShadow = true);

// Plain Win32 painting for hosts where per-pixel layered composition produces
// a black surface. The layout (including click targets) is shared with skins.
bool paintOpaqueCandidateSurface(HDC dc, const RECT& client, const std::wstring& face,
                                 const CandidateStyle& style, const std::vector<CandidateItem>& items,
                                 const CandidateLayout& layout, unsigned selected, int dpi);

} // namespace xmjd
