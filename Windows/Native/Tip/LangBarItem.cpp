#include "LangBarItem.h"

#include <objidl.h>
#include <olectl.h>
#include <ctffunc.h>
#include <gdiplus.h>

#include <utility>
#include <cstdio>

namespace xmjd::tip {

namespace {
constexpr wchar_t kItemText[] = L"星猫";
constexpr DWORD kLangBarItemSinkCookie = 1;

void logLangBarEvent(const char* event, DWORD flags, HRESULT result) noexcept {
    char line[192]{};
    std::snprintf(line, sizeof(line), "XMJD LangBar pid=%lu tid=%lu event=%s flags=0x%08lX hr=0x%08lX\n",
                  GetCurrentProcessId(), GetCurrentThreadId(), event,
                  static_cast<unsigned long>(flags), static_cast<unsigned long>(result));
    OutputDebugStringA(line);
}

BSTR wideBstr(const wchar_t* text) {
    return SysAllocString(text);
}

HICON makeModeIcon(const wchar_t* glyph, Gdiplus::ARGB background) {
    Gdiplus::GdiplusStartupInput startupInput;
    ULONG_PTR token = 0;
    if (Gdiplus::GdiplusStartup(&token, &startupInput, nullptr) != Gdiplus::Ok) return nullptr;
    HICON icon = nullptr;
    {
        constexpr INT size = 16;
        Gdiplus::Bitmap bitmap(size, size, PixelFormat32bppARGB);
        Gdiplus::Graphics graphics(&bitmap);
        if (bitmap.GetLastStatus() == Gdiplus::Ok && graphics.GetLastStatus() == Gdiplus::Ok) {
            graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            graphics.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
            graphics.Clear(Gdiplus::Color(0, 0, 0, 0));
            Gdiplus::SolidBrush circle{Gdiplus::Color(background)};
            graphics.FillEllipse(&circle, 0, 0, size - 1, size - 1);
            Gdiplus::Font font(L"Microsoft YaHei UI", 8.5f, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
            Gdiplus::SolidBrush text(Gdiplus::Color(255, 255, 255, 255));
            Gdiplus::StringFormat format;
            format.SetAlignment(Gdiplus::StringAlignmentCenter);
            format.SetLineAlignment(Gdiplus::StringAlignmentCenter);
            const Gdiplus::RectF bounds(0, -0.5f, static_cast<Gdiplus::REAL>(size), static_cast<Gdiplus::REAL>(size));
            if (font.GetLastStatus() == Gdiplus::Ok &&
                graphics.DrawString(glyph, -1, &font, bounds, &format, &text) == Gdiplus::Ok) {
                bitmap.GetHICON(&icon);
            }
        }
    }
    Gdiplus::GdiplusShutdown(token);
    return icon;
}
} // namespace

const GUID& LangBarItem::kItemGuid() { return GUID_LBI_INPUTMODE; }

LangBarItem::LangBarItem(HINSTANCE instance, CLSID service, std::function<void()> clicked)
    : instance_(instance), service_(service), clicked_(std::move(clicked)) {
    chineseIcon_ = makeModeIcon(L"中", Gdiplus::Color(255, 0xFF, 0x6B, 0x35).GetValue());
    englishIcon_ = makeModeIcon(L"英", Gdiplus::Color(255, 0x4A, 0x55, 0x68).GetValue());
    fallbackIcon_ = static_cast<HICON>(LoadImageW(instance_, MAKEINTRESOURCEW(1), IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR));
    fallbackIconOwned_ = fallbackIcon_ != nullptr;
    if (!fallbackIcon_) fallbackIcon_ = LoadIconW(nullptr, IDI_APPLICATION);
}

LangBarItem::~LangBarItem() {
    if (sink_) sink_->Release();
    if (chineseIcon_) DestroyIcon(chineseIcon_);
    if (englishIcon_) DestroyIcon(englishIcon_);
    if (fallbackIconOwned_ && fallbackIcon_) DestroyIcon(fallbackIcon_);
}

HRESULT STDMETHODCALLTYPE LangBarItem::QueryInterface(REFIID riid, void** object) {
    if (!object) return E_POINTER;
    *object = nullptr;
    if (riid == IID_IUnknown || riid == IID_ITfLangBarItem || riid == IID_ITfLangBarItemButton) {
        *object = static_cast<ITfLangBarItemButton*>(this);
    } else if (riid == IID_ITfSource) {
        *object = static_cast<ITfSource*>(this);
    } else {
        return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
}

ULONG STDMETHODCALLTYPE LangBarItem::AddRef() { return ++refs_; }
ULONG STDMETHODCALLTYPE LangBarItem::Release() {
    const ULONG remaining = --refs_;
    if (remaining == 0) delete this;
    return remaining;
}

HRESULT LangBarItem::setAsciiMode(bool asciiMode) {
    if (asciiMode_ != asciiMode) {
        asciiMode_ = asciiMode;
        pendingFlags_ |= TF_LBI_ICON | TF_LBI_TOOLTIP;
    }
    return notifyPending();
}

HRESULT LangBarItem::notifyPending() {
    if (!sink_ || !pendingFlags_ || notifying_) return S_OK;
    AddRef();
    notifying_ = true;
    HRESULT result = S_OK;
    while (sink_ && pendingFlags_) {
        const DWORD flags = std::exchange(pendingFlags_, 0);
        ITfLangBarItemSink* sink = sink_;
        sink->AddRef();
        const HRESULT hr = sink->OnUpdate(flags);
        sink->Release();
        logLangBarEvent("on-update", flags, hr);
        if (FAILED(hr)) {
            pendingFlags_ |= flags;
            result = hr;
            break;
        }
    }
    notifying_ = false;
    Release();
    return result;
}

HRESULT STDMETHODCALLTYPE LangBarItem::AdviseSink(REFIID riid, IUnknown* sink, DWORD* cookie) {
    if (!sink || !cookie) return E_INVALIDARG;
    *cookie = 0;
    if (riid != IID_ITfLangBarItemSink) return CONNECT_E_CANNOTCONNECT;
    if (sink_) return CONNECT_E_ADVISELIMIT;
    ITfLangBarItemSink* itemSink = nullptr;
    const HRESULT hr = sink->QueryInterface(IID_PPV_ARGS(&itemSink));
    if (FAILED(hr) || !itemSink) return CONNECT_E_CANNOTCONNECT;
    sink_ = itemSink;
    sinkCookie_ = kLangBarItemSinkCookie;
    *cookie = sinkCookie_;
    logLangBarEvent("sink-advised", 0, S_OK);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE LangBarItem::UnadviseSink(DWORD cookie) {
    if (!sink_ || cookie != sinkCookie_) return CONNECT_E_NOCONNECTION;
    ITfLangBarItemSink* sink = std::exchange(sink_, nullptr);
    sinkCookie_ = 0;
    sink->Release();
    logLangBarEvent("sink-unadvised", 0, S_OK);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE LangBarItem::GetInfo(TF_LANGBARITEMINFO* info) {
    if (!info) return E_POINTER;
    info->clsidService = service_;
    info->guidItem = GUID_LBI_INPUTMODE;
    info->dwStyle = TF_LBI_STYLE_BTN_BUTTON | TF_LBI_STYLE_HIDDENSTATUSCONTROL;
    info->ulSort = 0;
    wcscpy_s(info->szDescription, kItemText);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE LangBarItem::GetStatus(DWORD* status) {
    if (!status) return E_POINTER;
    *status = shown_ ? 0 : TF_LBI_STATUS_HIDDEN;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE LangBarItem::Show(BOOL show) {
    if (shown_ != (show != FALSE)) {
        shown_ = show != FALSE;
        pendingFlags_ |= TF_LBI_STATUS;
    }
    return notifyPending();
}

HRESULT STDMETHODCALLTYPE LangBarItem::GetTooltipString(BSTR* tooltip) {
    if (!tooltip) return E_POINTER;
    *tooltip = wideBstr(asciiMode_ ? L"星猫键道 · 英文模式 · 点击打开设置" : L"星猫键道 · 中文模式 · 点击打开设置");
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
    const HICON source = asciiMode_ ? (englishIcon_ ? englishIcon_ : fallbackIcon_)
                                    : (chineseIcon_ ? chineseIcon_ : fallbackIcon_);
    if (!source) {
        *icon = nullptr;
        return E_FAIL;
    }
    *icon = CopyIcon(source);
    if (*icon) return S_OK;
    const DWORD error = GetLastError();
    return HRESULT_FROM_WIN32(error ? error : ERROR_NOT_ENOUGH_MEMORY);
}

HRESULT STDMETHODCALLTYPE LangBarItem::GetText(BSTR* text) {
    if (!text) return E_POINTER;
    *text = wideBstr(kItemText);
    return *text ? S_OK : E_OUTOFMEMORY;
}

} // namespace xmjd::tip
