#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <msctf.h>
#include <ctffunc.h>
#include <objbase.h>

#include "LangBarItem.h"

#include <iostream>
#include <vector>

namespace {
using xmjd::tip::LangBarItem;

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            std::cerr << "Check failed: " #condition << " at " << __FILE__ << ':' << __LINE__ << '\n'; \
            return false; \
        } \
    } while (false)

class ItemSink final : public ITfLangBarItemSink {
public:
    explicit ItemSink(LangBarItem* item) : item_(item) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (iid != IID_IUnknown && iid != IID_ITfLangBarItemSink) return E_NOINTERFACE;
        *object = static_cast<ITfLangBarItemSink*>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = --refs_;
        if (remaining == 0) delete this;
        return remaining;
    }
    HRESULT STDMETHODCALLTYPE OnUpdate(DWORD flags) override {
        updates_.push_back(flags);
        if (reenter_ && updates_.size() == 1) {
            reenter_ = false;
            return item_->Show(FALSE);
        }
        return S_OK;
    }

    const std::vector<DWORD>& updates() const { return updates_; }
private:
    ~ItemSink() = default;
    ULONG refs_ = 1;
    LangBarItem* item_;
    bool reenter_ = true;
    std::vector<DWORD> updates_;
};

bool verifyLanguageBarIdentityAndIcons() {
    const CLSID service = {0x5f8f59f4, 0xe5a8, 0x4944, {0x96, 0x0b, 0x8a, 0x8a, 0xd3, 0xbe, 0x88, 0x57}};
    auto* item = new LangBarItem(GetModuleHandleW(nullptr), service, [] {});
    TF_LANGBARITEMINFO info{};
    CHECK(item->GetInfo(&info) == S_OK);
    CHECK(IsEqualGUID(info.guidItem, GUID_LBI_INPUTMODE));
    CHECK(IsEqualGUID(item->guid(), GUID_LBI_INPUTMODE));

    HICON first = nullptr;
    HICON second = nullptr;
    CHECK(item->GetIcon(&first) == S_OK && first);
    CHECK(item->GetIcon(&second) == S_OK && second);
    CHECK(first != second);
    CHECK(DestroyIcon(first));
    CHECK(DestroyIcon(second));
    item->Release();
    return true;
}

bool verifyReentrantNotifications() {
    const CLSID service = {0x5f8f59f4, 0xe5a8, 0x4944, {0x96, 0x0b, 0x8a, 0x8a, 0xd3, 0xbe, 0x88, 0x57}};
    auto* item = new LangBarItem(GetModuleHandleW(nullptr), service, [] {});
    auto* sink = new ItemSink(item);
    DWORD cookie = 0;
    CHECK(item->AdviseSink(IID_ITfLangBarItemSink, sink, &cookie) == S_OK);
    CHECK(cookie != 0);
    CHECK(sink->updates().empty());

    CHECK(item->setAsciiMode(true) == S_OK);
    CHECK(sink->updates().size() == 2);
    CHECK((sink->updates()[0] & (TF_LBI_ICON | TF_LBI_TOOLTIP)) == (TF_LBI_ICON | TF_LBI_TOOLTIP));
    CHECK((sink->updates()[1] & TF_LBI_STATUS) != 0);

    CHECK(item->UnadviseSink(cookie) == S_OK);
    const std::size_t updateCount = sink->updates().size();
    CHECK(item->setAsciiMode(false) == S_OK);
    CHECK(sink->updates().size() == updateCount);
    item->Release();
    sink->Release();
    return true;
}
} // namespace

int wmain() {
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized)) {
        std::cerr << "COM initialization failed: " << std::hex << initialized << '\n';
        return 1;
    }
    const bool passed = verifyLanguageBarIdentityAndIcons() && verifyReentrantNotifications();
    CoUninitialize();
    if (passed) std::cout << "PASS: language bar GUID, icon ownership and reentrant notifications verified.\n";
    return passed ? 0 : 1;
}
