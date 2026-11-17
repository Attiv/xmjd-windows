#include "LangBarItem.h"

#include <atomic>
#include <new>

namespace xmjd::tip {


namespace {
// 自己的 GUID：不用 GUID_LBI_INPUTMODE，那个由系统专门用于"输入模式图标"，
// 语义要求严，先按普通按钮注册，把我们的图标挂到输入指示器上。
constexpr GUID kLangBarItemGuid{0x7f3a1c62, 0x9d24, 0x4b58, {0xa1, 0x8f, 0x2c, 0x51, 0x77, 0x0e, 0x3d, 0x64}};
constexpr wchar_t kItemText[] = L"星猫";

BSTR wideBstr(const wchar_t* text) {
    return SysAllocString(text);
}
} // namespace

const GUID& LangBarItem::kItemGuid() { return kLangBarItemGuid; }

LangBarItem::LangBarItem(HINSTANCE instance, CLSID service, std::function<void()> clicked)
    : instance_(instance), service_(service), clicked_(std::move(clicked)) {
    // 16px 是输入指示器的标准尺寸；失败就退到系统默认图标，不影响其余功能。
    icon_ = static_cast<HICON>(LoadImageW(instance_, MAKEINTRESOURCEW(1), IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR));
    if (!icon_) icon_ = LoadIconW(nullptr, IDI_APPLICATION);
}

LangBarItem::~LangBarItem() {
    if (icon_) DestroyIcon(icon_);
}

HRESULT STDMETHODCALLTYPE LangBarItem::QueryInterface(REFIID riid, void** object) {
    if (!object) return E_POINTER;
    *object = nullptr;
    if (riid == IID_IUnknown || riid == IID_ITfLangBarItem || riid == IID_ITfLangBarItemButton) {
        *object = static_cast<ITfLangBarItemButton*>(this);
        AddRef();
        return S_OK;
    }
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE LangBarItem::AddRef() { return ++refs_; }
ULONG STDMETHODCALLTYPE LangBarItem::Release() {
    const ULONG remaining = --refs_;
    if (remaining == 0) delete this;
    return remaining;
}

HRESULT STDMETHODCALLTYPE LangBarItem::GetInfo(TF_LANGBARITEMINFO* info) {
    if (!info) return E_POINTER;
    info->clsidService = service_;
    info->guidItem = kLangBarItemGuid;
    info->dwStyle = TF_LBI_STYLE_BTN_BUTTON | TF_LBI_STYLE_SHOWNINTRAY;
    info->ulSort = 0;
    wcscpy_s(info->szDescription, kItemText);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE LangBarItem::GetStatus(DWORD* status) {
    if (!status) return E_POINTER;
    *status = 0;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE LangBarItem::Show(BOOL show) {
    shown_ = show != FALSE;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE LangBarItem::GetTooltipString(BSTR* tooltip) {
    if (!tooltip) return E_POINTER;
    *tooltip = wideBstr(L"星猫键道 · 点击打开设置");
    return *tooltip ? S_OK : E_OUTOFMEMORY;
}

HRESULT STDMETHODCALLTYPE LangBarItem::OnClick(TfLBIClick click, POINT, const RECT*) {
    if (click == TF_LBI_CLK_LEFT || click == TF_LBI_CLK_RIGHT) {
        if (clicked_) clicked_();
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE LangBarItem::InitMenu(ITfMenu*) {
    // 最小实现：不挂菜单，点击就是打开设置。菜单后续再加。
    return S_OK;
}

HRESULT STDMETHODCALLTYPE LangBarItem::OnMenuSelect(UINT) { return S_OK; }

HRESULT STDMETHODCALLTYPE LangBarItem::GetIcon(HICON* icon) {
    if (!icon) return E_POINTER;
    *icon = icon_;   // 所有权归本对象，调用方不释放
    return S_OK;
}

HRESULT STDMETHODCALLTYPE LangBarItem::GetText(BSTR* text) {
    if (!text) return E_POINTER;
    *text = wideBstr(kItemText);
    return *text ? S_OK : E_OUTOFMEMORY;
}

} // namespace xmjd::tip
