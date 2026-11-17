#pragma once

#include <windows.h>
#include <msctf.h>

#include <atomic>
#include <functional>

namespace xmjd::tip {

// 语言栏按钮：注册成 TSF 的 LangBar 项后，Windows 才把本输入法当作"正规 IME"对待，
// 输入指示器也才有我们的位置。只做最小实现——一个图标 + 点击打开设置，
// 菜单与中英图标后续再补，避免一次引入过多 COM 表面。
class LangBarItem final : public ITfLangBarItemButton {
public:
    LangBarItem(HINSTANCE instance, CLSID service, std::function<void()> clicked);
    const GUID& guid() const { return kItemGuid(); }
    static const GUID& kItemGuid();

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;

    // ITfLangBarItem
    HRESULT STDMETHODCALLTYPE GetInfo(TF_LANGBARITEMINFO* info) override;
    HRESULT STDMETHODCALLTYPE GetStatus(DWORD* status) override;
    HRESULT STDMETHODCALLTYPE Show(BOOL show) override;
    HRESULT STDMETHODCALLTYPE GetTooltipString(BSTR* tooltip) override;

    // ITfLangBarItemButton
    HRESULT STDMETHODCALLTYPE OnClick(TfLBIClick click, POINT point, const RECT* rect) override;
    HRESULT STDMETHODCALLTYPE InitMenu(ITfMenu* menu) override;
    HRESULT STDMETHODCALLTYPE OnMenuSelect(UINT id) override;
    HRESULT STDMETHODCALLTYPE GetIcon(HICON* icon) override;
    HRESULT STDMETHODCALLTYPE GetText(BSTR* text) override;

private:
    ~LangBarItem();

    std::atomic<ULONG> refs_{1};
    HINSTANCE instance_;
    CLSID service_;
    std::function<void()> clicked_;
    bool shown_ = true;
    HICON icon_ = nullptr;
};

} // namespace xmjd::tip
