#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <msctf.h>
#include <objidl.h>
#include <wrl/client.h>

#include <iostream>
#include <type_traits>

namespace {
using Microsoft::WRL::ComPtr;
using GetClassObject = HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, void**);
using CanUnloadNow = HRESULT(STDAPICALLTYPE*)();

constexpr CLSID kTipClsid = {0x5f8f59f4, 0xe5a8, 0x4944, {0x96, 0x0b, 0x8a, 0x8a, 0xd3, 0xbe, 0x88, 0x57}};
constexpr CLSID kUnknownClsid = {0x38bc6200, 0x292a, 0x45cf, {0x9b, 0xb4, 0xa2, 0x2e, 0x57, 0x34, 0x9c, 0x77}};

// 以实际 Windows SDK 继承关系为准，避免 TIP 重复继承基础接口。
static_assert(std::is_base_of_v<ITfTextInputProcessor, ITfTextInputProcessorEx>);
static_assert(std::is_base_of_v<IUnknown, ITfTextInputProcessorEx>);

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            std::cerr << "检查失败: " #condition << "，位置 " << __FILE__ << ':' << __LINE__ << '\n'; \
            return false; \
        } \
    } while (false)

class OuterUnknown final : public IUnknown {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (iid != IID_IUnknown) return E_NOINTERFACE;
        *object = static_cast<IUnknown*>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override { return --refs_; }
private:
    ULONG refs_ = 1;
};

class ActivationManager final : public ITfThreadMgrEx, public ITfKeystrokeMgr,
                                public ITfUIElementMgr, public ITfSource {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (iid == IID_IUnknown || iid == IID_ITfThreadMgr || iid == IID_ITfThreadMgrEx) {
            *object = static_cast<ITfThreadMgrEx*>(this);
        } else if (iid == IID_ITfKeystrokeMgr) *object = static_cast<ITfKeystrokeMgr*>(this);
        else if (iid == IID_ITfUIElementMgr) *object = static_cast<ITfUIElementMgr*>(this);
        else if (iid == IID_ITfSource) *object = static_cast<ITfSource*>(this);
        else return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override { return --refs; }
    HRESULT STDMETHODCALLTYPE Activate(TfClientId*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE ActivateEx(TfClientId*, DWORD) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Deactivate() override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetActiveFlags(DWORD* flags) override {
        if (!flags) return E_POINTER;
        ++activeFlagsQueries;
        *flags = activeFlags;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE CreateDocumentMgr(ITfDocumentMgr**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE EnumDocumentMgrs(IEnumTfDocumentMgrs**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetFocus(ITfDocumentMgr** document) override { *document = nullptr; return S_OK; }
    HRESULT STDMETHODCALLTYPE SetFocus(ITfDocumentMgr*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE AssociateFocus(HWND, ITfDocumentMgr*, ITfDocumentMgr**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE IsThreadFocus(BOOL*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetFunctionProvider(REFCLSID, ITfFunctionProvider**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE EnumFunctionProviders(IEnumTfFunctionProviders**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetGlobalCompartment(ITfCompartmentMgr**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE AdviseKeyEventSink(TfClientId, ITfKeyEventSink* sink, BOOL foreground) override {
        if (!sink || !foreground || keySink) return E_UNEXPECTED;
        keySink = sink;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE UnadviseKeyEventSink(TfClientId) override { keySink.Reset(); return S_OK; }
    HRESULT STDMETHODCALLTYPE GetForeground(CLSID*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE TestKeyDown(WPARAM, LPARAM, BOOL*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE TestKeyUp(WPARAM, LPARAM, BOOL*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE KeyDown(WPARAM, LPARAM, BOOL*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE KeyUp(WPARAM, LPARAM, BOOL*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetPreservedKey(ITfContext*, const TF_PRESERVEDKEY*, GUID*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE IsPreservedKey(REFGUID, const TF_PRESERVEDKEY*, BOOL*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE PreserveKey(TfClientId, REFGUID, const TF_PRESERVEDKEY*, const WCHAR*, ULONG) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE UnpreserveKey(REFGUID, const TF_PRESERVEDKEY*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPreservedKeyDescription(REFGUID, const WCHAR*, ULONG) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetPreservedKeyDescription(REFGUID, BSTR*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SimulatePreservedKey(ITfContext*, REFGUID, BOOL*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE BeginUIElement(ITfUIElement*, BOOL*, DWORD*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE UpdateUIElement(DWORD) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE EndUIElement(DWORD) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetUIElement(DWORD, ITfUIElement**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE EnumUIElements(IEnumTfUIElements**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE AdviseSink(REFIID iid, IUnknown* sink, DWORD* cookie) override {
        if (!sink || !cookie) return E_INVALIDARG;
        if (iid == IID_ITfThreadMgrEventSink && !threadSink) {
            if (failThreadAdvice) return E_FAIL;
            threadSink = sink;
            ++threadAdviceCount;
            *cookie = 8;
            return S_OK;
        }
        if (iid != IID_ITfInputProcessorProfileActivationSink || profileSink) return E_UNEXPECTED;
        if (failProfileAdvice) return E_FAIL;
        profileSink = sink;
        *cookie = 7;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE UnadviseSink(DWORD cookie) override {
        if (cookie == 8 && threadSink) { threadSink.Reset(); return S_OK; }
        if (cookie != 7 || !profileSink) return E_INVALIDARG;
        profileSink.Reset();
        return S_OK;
    }
    ULONG refs = 1;
    DWORD activeFlags = TF_TMF_UIELEMENTENABLEDONLY;
    UINT activeFlagsQueries = 0;
    bool failProfileAdvice = false;
    bool failThreadAdvice = false;
    UINT threadAdviceCount = 0;
    ComPtr<ITfKeyEventSink> keySink;
    ComPtr<IUnknown> profileSink;
    ComPtr<IUnknown> threadSink;
};

bool verifyActivation(IClassFactory* factory, HMODULE module) {
    ComPtr<ITfTextInputProcessorEx> tip;
    CHECK(factory->CreateInstance(nullptr, IID_ITfTextInputProcessorEx,
                                  reinterpret_cast<void**>(tip.GetAddressOf())) == S_OK);
    ActivationManager manager;
    manager.failThreadAdvice = true;
    CHECK(tip->ActivateEx(&manager, 1, 0) == E_FAIL);
    CHECK(manager.threadAdviceCount == 0);
    CHECK(!manager.keySink && !manager.profileSink && !manager.threadSink && manager.refs == 1);
    manager.failThreadAdvice = false;
    manager.failProfileAdvice = true;
    CHECK(tip->ActivateEx(&manager, 1, 0) == E_FAIL);
    CHECK(manager.threadAdviceCount == 1);
    CHECK(!manager.keySink && !manager.profileSink && !manager.threadSink && manager.refs == 1);
    manager.failProfileAdvice = false;
    for (DWORD flags : {DWORD{0}, static_cast<DWORD>(TF_TMF_UIELEMENTENABLEDONLY)}) {
        manager.activeFlags = flags;
        CHECK(tip->ActivateEx(&manager, 1, 0) == S_OK);
        CHECK(manager.keySink && manager.profileSink && manager.threadSink);
        CHECK(tip->ActivateEx(&manager, 1, 0) == TF_E_ALREADY_EXISTS);
        WNDCLASSEXW popup{sizeof(popup)};
        // UI-less 仍注册隐藏窗口，定时恢复不依赖候选可见性。
        CHECK(GetClassInfoExW(module, L"XMJD.TSF.CandidatePopup.v1", &popup));
        CHECK(popup.hInstance == module);
        CHECK(tip->Deactivate() == S_OK);
        CHECK(tip->Deactivate() == S_OK);
        CHECK(!manager.keySink && !manager.profileSink && !manager.threadSink && manager.refs == 1);
    }
    // 候选由谁绘制只由 BeginUIElement 回填的 pbShow 决定；UILess 标志
    // （activeFlags）不再参与判断，所以这里不应再去查询 GetActiveFlags。
    CHECK(manager.activeFlagsQueries == 0);
    CHECK(manager.threadAdviceCount == 3);
    return true;
}

bool verifyInterfaces(IClassFactory* factory, CanUnloadNow canUnload) {
    ComPtr<ITfTextInputProcessor> base;
    CHECK(factory->CreateInstance(nullptr, IID_ITfTextInputProcessor,
                                  reinterpret_cast<void**>(base.GetAddressOf())) == S_OK);
    CHECK(base);
    CHECK(canUnload() == S_FALSE);

    ComPtr<ITfTextInputProcessorEx> extended;
    CHECK(base.As(&extended) == S_OK);
    CHECK(extended);
    CHECK(static_cast<ITfTextInputProcessor*>(extended.Get()) == base.Get());
    ComPtr<ITfTextInputProcessor> baseFromExtended;
    CHECK(extended.As(&baseFromExtended) == S_OK);
    CHECK(baseFromExtended.Get() == base.Get());

    ComPtr<IUnknown> identity;
    ComPtr<IUnknown> extendedIdentity;
    CHECK(base.As(&identity) == S_OK);
    CHECK(extended.As(&extendedIdentity) == S_OK);
    CHECK(identity.Get() == extendedIdentity.Get());

    ComPtr<ITfKeyEventSink> keys;
    ComPtr<ITfCompositionSink> compositions;
    ComPtr<ITfThreadMgrEventSink> threadEvents;
    CHECK(base.As(&threadEvents) == S_OK);
    CHECK(threadEvents->OnInitDocumentMgr(nullptr) == S_OK);
    CHECK(threadEvents->OnUninitDocumentMgr(nullptr) == S_OK);
    CHECK(threadEvents->OnSetFocus(nullptr, nullptr) == S_OK);
    CHECK(threadEvents->OnPushContext(nullptr) == S_OK);
    CHECK(threadEvents->OnPopContext(nullptr) == S_OK);
    CHECK(base.As(&keys) == S_OK);
    CHECK(base.As(&compositions) == S_OK);
    ComPtr<IUnknown> keyIdentity;
    ComPtr<IUnknown> compositionIdentity;
    CHECK(keys.As(&keyIdentity) == S_OK);
    CHECK(compositions.As(&compositionIdentity) == S_OK);
    CHECK(keyIdentity.Get() == identity.Get());
    CHECK(compositionIdentity.Get() == identity.Get());
    ComPtr<ITfInputProcessorProfileActivationSink> profiles;
    ComPtr<IUnknown> profileIdentity;
    CHECK(base.As(&profiles) == S_OK);
    CHECK(profiles.As(&profileIdentity) == S_OK);
    CHECK(profileIdentity.Get() == identity.Get());
    CHECK(base->Activate(nullptr, 1) == E_INVALIDARG);
    CHECK(extended->ActivateEx(nullptr, 1, 0) == E_INVALIDARG);
    CHECK(base->Deactivate() == S_OK);
    CHECK(base->Deactivate() == S_OK);
    BOOL eaten = TRUE;
    CHECK(keys->OnTestKeyDown(nullptr, 'A', 0, &eaten) == S_OK);
    CHECK(eaten == FALSE);
    CHECK(keys->OnKeyDown(nullptr, 'A', 0, &eaten) == S_OK);
    CHECK(eaten == FALSE);
    CHECK(keys->OnKeyDown(nullptr, 'A', 0, nullptr) == E_POINTER);
    CHECK(keys->OnTestKeyUp(nullptr, VK_CAPITAL, 0, nullptr) == E_POINTER);
    CHECK(compositions->OnCompositionTerminated(0, nullptr) == S_OK);

    ComPtr<IStream> unsupported;
    CHECK(base.As(&unsupported) == E_NOINTERFACE);
    CHECK(!unsupported);
    CHECK(base->QueryInterface(IID_IUnknown, nullptr) == E_POINTER);

    ComPtr<ITfTextInputProcessorEx> createdExtended;
    CHECK(factory->CreateInstance(nullptr, IID_ITfTextInputProcessorEx,
                                  reinterpret_cast<void**>(createdExtended.GetAddressOf())) == S_OK);
    CHECK(createdExtended);
    ComPtr<ITfTextInputProcessor> createdBase;
    CHECK(createdExtended.As(&createdBase) == S_OK);

    ComPtr<IUnknown> rejected;
    CHECK(factory->CreateInstance(nullptr, IID_IStream,
                                  reinterpret_cast<void**>(rejected.GetAddressOf())) == E_NOINTERFACE);
    CHECK(!rejected);
    OuterUnknown outer;
    CHECK(factory->CreateInstance(&outer, IID_IUnknown,
                                  reinterpret_cast<void**>(rejected.GetAddressOf())) == CLASS_E_NOAGGREGATION);
    CHECK(!rejected);
    CHECK(factory->CreateInstance(nullptr, IID_IUnknown, nullptr) == E_POINTER);
    return true;
}

bool verifyExports(HMODULE module) {
    const auto getClassObject = reinterpret_cast<GetClassObject>(GetProcAddress(module, "DllGetClassObject"));
    const auto canUnload = reinterpret_cast<CanUnloadNow>(GetProcAddress(module, "DllCanUnloadNow"));
    CHECK(getClassObject);
    CHECK(canUnload);
    CHECK(GetProcAddress(module, "DllRegisterServer"));
    CHECK(GetProcAddress(module, "DllUnregisterServer"));
    CHECK(canUnload() == S_OK);

    ComPtr<IClassFactory> factory;
    CHECK(getClassObject(kUnknownClsid, IID_IClassFactory,
                         reinterpret_cast<void**>(factory.GetAddressOf())) == CLASS_E_CLASSNOTAVAILABLE);
    CHECK(!factory);
    CHECK(getClassObject(kTipClsid, IID_IClassFactory,
                         reinterpret_cast<void**>(factory.GetAddressOf())) == S_OK);
    CHECK(factory);
    CHECK(canUnload() == S_FALSE);
    ComPtr<IStream> unsupported;
    CHECK(getClassObject(kTipClsid, IID_IStream,
                         reinterpret_cast<void**>(unsupported.GetAddressOf())) == E_NOINTERFACE);
    CHECK(!unsupported);
    CHECK(getClassObject(kTipClsid, IID_IUnknown, nullptr) == E_POINTER);
    CHECK(factory->LockServer(TRUE) == S_OK);
    CHECK(factory->LockServer(FALSE) == S_OK);

    CHECK(verifyInterfaces(factory.Get(), canUnload));
    CHECK(verifyActivation(factory.Get(), module));
    factory.Reset();
    CHECK(canUnload() == S_OK);
    WNDCLASSEXW popup{sizeof(popup)};
    CHECK(!GetClassInfoExW(module, L"XMJD.TSF.CandidatePopup.v1", &popup));

    CHECK(getClassObject(kTipClsid, IID_IClassFactory,
                         reinterpret_cast<void**>(factory.GetAddressOf())) == S_OK);
    CHECK(factory->LockServer(TRUE) == S_OK);
    factory.Reset();
    CHECK(canUnload() == S_FALSE);
    CHECK(getClassObject(kTipClsid, IID_IClassFactory,
                         reinterpret_cast<void**>(factory.GetAddressOf())) == S_OK);
    CHECK(factory->LockServer(FALSE) == S_OK);
    factory.Reset();
    CHECK(canUnload() == S_OK);
    return true;
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) {
        std::wcerr << L"用法: XMJDTipComTests <当前架构的 XMJDTip.dll 绝对路径>\n";
        return 2;
    }
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized)) {
        std::cerr << "COM 初始化失败: " << std::hex << initialized << '\n';
        return 1;
    }
    bool passed = true;
    // 连续卸载重载会捕获遗留窗口类中的旧 DLL WndProc。
    for (int round = 0; round < 2 && passed; ++round) {
        const HMODULE module = LoadLibraryExW(argv[1], nullptr,
                                             LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!module) {
            std::cerr << "加载 TIP 失败: " << GetLastError() << '\n';
            passed = false;
            break;
        }
        passed = verifyExports(module);
        if (!FreeLibrary(module)) passed = false;
    }
    CoUninitialize();
    if (passed) std::cout << "TIP COM 接口检查通过；尚未验证 TSF 注册和真实输入。\n";
    return passed ? 0 : 1;
}
