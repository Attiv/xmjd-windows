#pragma once

#include <windows.h>
#include <msctf.h>

#include <atomic>
#include <functional>

namespace xmjd::tip {

// TSF 语言栏按钮：提供设置入口，并按引擎状态显示中/英模式图标。
class LangBarItem final : public ITfLangBarItemButton, public ITfSource {
public:
    LangBarItem(HINSTANCE instance, CLSID service, std::function<void()> clicked);
    const GUID& guid() const { return kItemGuid(); }
    static const GUID& kItemGuid();
    HRESULT setAsciiMode(bool asciiMode);
    HRESULT STDMETHODCALLTYPE AdviseSink(REFIID riid, IUnknown* sink, DWORD* cookie) override;
    HRESULT STDMETHODCALLTYPE UnadviseSink(DWORD cookie) override;

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
    bool asciiMode_ = false;
    HRESULT notifyPending();
    DWORD pendingFlags_ = 0;
    bool notifying_ = false;
    HICON chineseIcon_ = nullptr;
    HICON englishIcon_ = nullptr;
    HICON fallbackIcon_ = nullptr;
    bool fallbackIconOwned_ = false;
    ITfLangBarItemSink* sink_ = nullptr;
    DWORD sinkCookie_ = 0;
};

} // namespace xmjd::tip
