#include "CandidateComposition.h"

namespace xmjd::tip {
using Microsoft::WRL::ComPtr;

HRESULT CandidateComposition::attach(HWND window) noexcept {
    ComPtr<ID3D11Device> d3d;
    auto create = [&](D3D_DRIVER_TYPE type) {
        return D3D11CreateDevice(nullptr, type, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                 nullptr, 0, D3D11_SDK_VERSION, d3d.ReleaseAndGetAddressOf(),
                                 nullptr, context_.ReleaseAndGetAddressOf());
    };
    HRESULT hr = create(D3D_DRIVER_TYPE_HARDWARE);
    if (FAILED(hr)) hr = create(D3D_DRIVER_TYPE_WARP);
    if (FAILED(hr)) return hr;

    ComPtr<IDXGIDevice> dxgi;
    hr = d3d.As(&dxgi);
    if (SUCCEEDED(hr)) hr = DCompositionCreateDevice(dxgi.Get(), IID_PPV_ARGS(&device_));
    if (SUCCEEDED(hr)) hr = device_->CreateTargetForHwnd(window, TRUE, &target_);
    if (SUCCEEDED(hr)) hr = device_->CreateVisual(&visual_);
    if (SUCCEEDED(hr)) hr = target_->SetRoot(visual_.Get());
    if (SUCCEEDED(hr)) window_ = window;
    return hr;
}

HRESULT CandidateComposition::present(HWND window, UINT width, UINT height, const void* pixels) noexcept {
    if (!window || !width || !height || !pixels || width > 8192 || height > 8192) return E_INVALIDARG;
    if (window_ && window_ != window) return E_INVALIDARG;
    if (!window_) {
        const HRESULT hr = attach(window);
        if (FAILED(hr)) return hr;
    }
    if (!surface_ || width != width_ || height != height_) {
        ComPtr<IDCompositionSurface> surface;
        HRESULT hr = device_->CreateSurface(width, height, DXGI_FORMAT_B8G8R8A8_UNORM,
                                            DXGI_ALPHA_MODE_PREMULTIPLIED, &surface);
        if (SUCCEEDED(hr)) hr = visual_->SetContent(surface.Get());
        if (FAILED(hr)) return hr;
        surface_ = surface;
        width_ = width;
        height_ = height;
    }

    ComPtr<IDXGISurface> update;
    POINT offset{};
    HRESULT hr = surface_->BeginDraw(nullptr, IID_PPV_ARGS(&update), &offset);
    if (FAILED(hr)) return hr;
    ComPtr<ID3D11Texture2D> texture;
    hr = update.As(&texture);
    if (SUCCEEDED(hr)) {
        D3D11_TEXTURE2D_DESC description{};
        texture->GetDesc(&description);
        const auto left = static_cast<UINT>(offset.x), top = static_cast<UINT>(offset.y);
        if (offset.x < 0 || offset.y < 0 || left > description.Width || top > description.Height ||
            width > description.Width - left || height > description.Height - top) {
            hr = E_INVALIDARG;
        } else {
            const D3D11_BOX box{left, top, 0, left + width, top + height, 1};
            context_->UpdateSubresource(texture.Get(), 0, &box, pixels, width * 4, 0);
        }
    }
    texture.Reset();
    update.Reset();
    const HRESULT ended = surface_->EndDraw();
    if (FAILED(hr)) return hr;
    if (FAILED(ended)) return ended;
    return device_->Commit();
}

HRESULT CandidateComposition::waitForCommit() const noexcept {
    return device_ ? device_->WaitForCommitCompletion() : E_UNEXPECTED;
}
} // namespace xmjd::tip
