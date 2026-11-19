#pragma once

#include <d3d11.h>
#include <dcomp.h>
#include <wrl/client.h>

namespace xmjd::tip {
// Presents the same premultiplied bitmap as the desktop layered window, but
// without using UpdateLayeredWindow in SearchHost.
class CandidateComposition {
public:
    HRESULT present(HWND window, UINT width, UINT height, const void* pixels) noexcept;
    HRESULT waitForCommit() const noexcept;

private:
    HRESULT attach(HWND window) noexcept;
    HWND window_ = nullptr;
    UINT width_ = 0;
    UINT height_ = 0;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<IDCompositionDevice> device_;
    Microsoft::WRL::ComPtr<IDCompositionTarget> target_;
    Microsoft::WRL::ComPtr<IDCompositionVisual> visual_;
    Microsoft::WRL::ComPtr<IDCompositionSurface> surface_;
};
} // namespace xmjd::tip
