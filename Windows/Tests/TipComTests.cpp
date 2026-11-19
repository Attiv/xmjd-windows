#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <aclapi.h>
#include <msctf.h>
#include "../Native/Common/Pipe.h"
#include "../Native/Tip/ShiftTap.h"
#include "../Native/Tip/CandidatePopup.h"
#include "../Native/Tip/CandidateComposition.h"
#include "../Native/Tip/CandidateRenderer.h"
#include "../Native/Tip/Tip.h"
#include "../Native/Common/CandidatePainter.h"
#include <objidl.h>
#include <sddl.h>
#include <tlhelp32.h>
#include <userenv.h>
#include <wrl/client.h>

#include <iostream>
#include <string_view>
#include <thread>
#include <type_traits>
#include <vector>

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
class HandleGuard final {
public:
    explicit HandleGuard(HANDLE handle = INVALID_HANDLE_VALUE) : handle_(handle) {}
    ~HandleGuard() { if (handle_ != INVALID_HANDLE_VALUE && handle_ != nullptr) CloseHandle(handle_); }
    HandleGuard(const HandleGuard&) = delete;
    HandleGuard& operator=(const HandleGuard&) = delete;
    HANDLE get() const { return handle_; }
private:
    HANDLE handle_;
};

bool verifyPipeSecurity() {
    const std::wstring name = L"\\\\.\\pipe\\XMJD\\security-test-" +
        std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
    HandleGuard server(xmjd::win::createServerPipe(true, name));
    CHECK(server.get() != INVALID_HANDLE_VALUE);

    PSID owner = nullptr;
    PACL dacl = nullptr;
    PACL sacl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const DWORD securityResult = GetSecurityInfo(server.get(), SE_KERNEL_OBJECT,
        OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION | LABEL_SECURITY_INFORMATION,
        &owner, nullptr, &dacl, &sacl, &descriptor);
    const auto checkDescriptor = [&]() {
        if (!IsValidSid(owner) || !dacl || !IsValidAcl(dacl) || !sacl || !IsValidAcl(sacl)) return false;
        SID ownerRightsSid{};
        SID_IDENTIFIER_AUTHORITY creatorAuthority = SECURITY_CREATOR_SID_AUTHORITY;
        if (!InitializeSid(&ownerRightsSid, &creatorAuthority, 1)) return false;
        *GetSidSubAuthority(&ownerRightsSid, 0) = 4;
        BYTE packageBuffer[SECURITY_MAX_SID_SIZE]{};
        DWORD packageSize = sizeof(packageBuffer);
        if (!CreateWellKnownSid(WinBuiltinAnyPackageSid, nullptr, packageBuffer, &packageSize)) return false;
        if (xmjd::win::allowPipeServerIdentityQuery(packageBuffer) != ERROR_INVALID_SID) return false;

        HANDLE rawToken = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &rawToken)) return false;
        HandleGuard token(rawToken);
        DWORD tokenBytes = 0;
        GetTokenInformation(token.get(), TokenLogonSid, nullptr, 0, &tokenBytes);
        if (tokenBytes == 0) return false;
        std::vector<BYTE> tokenStorage(tokenBytes);
        if (!GetTokenInformation(token.get(), TokenLogonSid, tokenStorage.data(), tokenBytes, &tokenBytes)) return false;
        const auto* logon = reinterpret_cast<const TOKEN_GROUPS*>(tokenStorage.data());
        if (logon->GroupCount == 0) return false;
        PSID logonSid = logon->Groups[0].Sid;

        bool ownerRightsLimited = false;
        bool packageAceFound = false;
        bool logonCreateInstance = false;
        for (DWORD i = 0; i < dacl->AceCount; ++i) {
            void* rawAce = nullptr;
            if (!GetAce(dacl, i, &rawAce)) return false;
            const auto* header = static_cast<const ACE_HEADER*>(rawAce);
            if (header->AceType != ACCESS_ALLOWED_ACE_TYPE) continue;
            const auto* ace = static_cast<const ACCESS_ALLOWED_ACE*>(rawAce);
            PSID sid = const_cast<DWORD*>(&ace->SidStart);
            if (!IsValidSid(sid)) return false;
            if (EqualSid(sid, &ownerRightsSid)) {
                ownerRightsLimited = ace->Mask == READ_CONTROL;
            } else if (EqualSid(sid, packageBuffer)) {
                packageAceFound = true;
                if ((ace->Mask & (FILE_CREATE_PIPE_INSTANCE | WRITE_DAC | WRITE_OWNER)) != 0) return false;
            } else if (EqualSid(sid, logonSid)) {
                logonCreateInstance = (ace->Mask & FILE_CREATE_PIPE_INSTANCE) != 0;
            }
        }
        if (!ownerRightsLimited || !packageAceFound || !logonCreateInstance) return false;

        bool lowLabelFound = false;
        for (DWORD i = 0; i < sacl->AceCount; ++i) {
            void* rawAce = nullptr;
            if (!GetAce(sacl, i, &rawAce)) return false;
            const auto* header = static_cast<const ACE_HEADER*>(rawAce);
            if (header->AceType != SYSTEM_MANDATORY_LABEL_ACE_TYPE) continue;
            const auto* ace = static_cast<const SYSTEM_MANDATORY_LABEL_ACE*>(rawAce);
            PSID sid = const_cast<DWORD*>(&ace->SidStart);
            if (!IsValidSid(sid)) return false;
            if (*GetSidSubAuthorityCount(sid) > 0 &&
                *GetSidSubAuthority(sid, *GetSidSubAuthorityCount(sid) - 1) == SECURITY_MANDATORY_LOW_RID) {
                lowLabelFound = true;
            }
        }
        if (!lowLabelFound) return false;

        const DWORD clientAccess = (FILE_GENERIC_READ | FILE_GENERIC_WRITE) & ~FILE_CREATE_PIPE_INSTANCE;
        HandleGuard client(CreateFileW(name.c_str(), clientAccess, 0, nullptr, OPEN_EXISTING, 0, nullptr));
        if (client.get() == INVALID_HANDLE_VALUE) return false;
        HandleGuard second(xmjd::win::createServerPipe(false, name));
        return second.get() != INVALID_HANDLE_VALUE;
    };
    const bool securityPassed = securityResult == ERROR_SUCCESS && checkDescriptor();
    if (descriptor) LocalFree(descriptor);
    CHECK(securityPassed);
    std::cout << "PASS: production pipe DACL, Low label and client access verified.\n";
    return true;
}

bool verifyOpaqueCandidatePaint() {
    constexpr int width = 180, height = 52;
    const HDC dc = CreateCompatibleDC(nullptr);
    CHECK(dc);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    const HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    const HGDIOBJ previous = bitmap ? SelectObject(dc, bitmap) : nullptr;
    const bool usable = previous && previous != HGDI_ERROR && bits;
    bool painted = false;
    bool background = false;
    bool readableText = false;
    if (usable) {
        xmjd::CandidateStyle style{};
        style.fontSize = 17;
        const RECT client{0, 0, width, height};
        const std::vector<xmjd::CandidateItem> items{{L"1", L"MAC", L""}, {L"2", L"Candidate", L""}};
        xmjd::CandidateLayout layout{};
        layout.width = width;
        layout.height = height;
        layout.items = {{3, 2, 174, 23}, {3, 26, 174, 23}};
        painted = xmjd::paintOpaqueCandidateSurface(dc, client, L"Segoe UI", style, items, layout, 0, 96);
        const COLORREF bg = GetSysColor(COLOR_WINDOW);
        background = GetPixel(dc, 176, 45) == bg;
        for (int y = 27; y < 48 && !readableText; ++y) {
            for (int x = 10; x < 120; ++x) {
                const COLORREF pixel = GetPixel(dc, x, y);
                if (pixel != CLR_INVALID && pixel != bg) { readableText = true; break; }
            }
        }
    }
    if (previous && previous != HGDI_ERROR) SelectObject(dc, previous);
    if (bitmap) DeleteObject(bitmap);
    DeleteDC(dc);
    CHECK(usable && painted && background && readableText);
    return true;
}

bool verifyCandidatePopup(bool checkComposition = false) {
    using namespace xmjd::tip;
    struct Window {
        HWND handle;
        ~Window() { if (IsWindow(handle)) DestroyWindow(handle); }
    };
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    const Window host{CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC", L"", WS_POPUP,
                                     0, 0, 1, 1, nullptr, nullptr, instance, nullptr)};
    CHECK(host.handle);
    const Window child{CreateWindowExW(0, L"STATIC", L"", WS_CHILD,
                                      0, 0, 1, 1, host.handle, nullptr, instance, nullptr)};
    CHECK(child.handle);
    auto owners = candidateOwnerChoices(child.handle, child.handle, host.handle);
    CHECK(owners[0] == host.handle && !owners[1] && !owners[2]);
    owners = candidateOwnerChoices(nullptr, nullptr, host.handle);
    CHECK(!owners[0] && !owners[1] && owners[2] == host.handle);
    CHECK(!candidateOwnerRoot(GetDesktopWindow()));
    CHECK(usableCandidateCaret(RECT{100, 100, 100, 118}));
    CHECK(!usableCandidateCaret(RECT{}));
    const Window popup{createCandidatePopup(L"STATIC", instance, owners[2], nullptr)};
    CHECK(popup.handle && GetWindow(popup.handle, GW_OWNER) == host.handle);
    CHECK((GetWindowLongPtrW(popup.handle, GWL_EXSTYLE) & (WS_EX_NOACTIVATE | WS_EX_LAYERED)) ==
          (WS_EX_NOACTIVATE | WS_EX_LAYERED));
    const Window opaque{createCandidatePopup(L"STATIC", instance, owners[2], nullptr, CandidatePresentation::Opaque)};
    CHECK(opaque.handle && GetWindow(opaque.handle, GW_OWNER) == host.handle);
    CHECK((GetWindowLongPtrW(opaque.handle, GWL_EXSTYLE) & WS_EX_LAYERED) == 0);
    if (checkComposition) {
        const Window composed{createCandidatePopup(L"STATIC", instance, owners[2], nullptr, CandidatePresentation::Composition)};
        CHECK(composed.handle && GetWindow(composed.handle, GW_OWNER) == host.handle);
        CHECK((GetWindowLongPtrW(composed.handle, GWL_EXSTYLE) & (WS_EX_LAYERED | WS_EX_NOREDIRECTIONBITMAP)) ==
              WS_EX_NOREDIRECTIONBITMAP);
        {
            CandidateComposition composition;
            const std::vector<DWORD> pixels(96 * 48, 0x80402010);
            for (UINT width : {80u, 80u, 96u}) {
                const HRESULT result = composition.present(composed.handle, width, 48, pixels.data());
                if (FAILED(result)) std::cerr << "CandidateComposition: " << std::hex << result << '\n';
                CHECK(SUCCEEDED(result));
            }
            CHECK(SUCCEEDED(composition.waitForCommit()));
        }
        CandidateRenderer renderer;
        auto skin = xmjd::builtinCandidateStyles().front();
        skin.id = "peer-probe";
        skin.horizontal = true;
        skin.opacity = 0.85;
        skin.fontSize = 22;
        renderer.setAppearance(std::move(skin), true);
        const std::vector<CandidateValue> candidates{{L"1", L"你", L""}, {L"2", L"MAC", L""}};
        CHECK(renderer.prepare(candidates, 0, 9, 96));
        CHECK(renderer.style().id == "peer-probe");
        CHECK(renderer.layout().items.size() == 2 && renderer.layout().items[0].y == renderer.layout().items[1].y);
        CHECK(renderer.shadowMargin() > 0);
        CHECK(SUCCEEDED(renderer.paintComposited(composed.handle)));
    }
    CHECK(DestroyWindow(host.handle));
    CHECK(!IsWindow(popup.handle) && !IsWindow(opaque.handle));
    return verifyOpaqueCandidatePaint();
}

bool verifyShiftTap() {
    xmjd::tip::ShiftTap tap;
    constexpr auto left = xmjd::kLeftShiftKey;
    constexpr auto right = xmjd::kRightShiftKey;
    CHECK(!tap.release(left, 100, false));
    tap.press(left, 100, false, false);
    tap.press(left, 120, true, false);
    CHECK(tap.release(left, 150, false));
    CHECK(!tap.release(left, 151, false));
    tap.press(right, 200, false, false);
    CHECK(tap.release(right, 240, false));
    tap.press(left, 300, false, false);
    tap.observeKeyDown(0); // An application-owned letter or shortcut.
    tap.press(left, 310, true, false);
    CHECK(!tap.release(left, 320, false));
    tap.press(left, 330, false, true);
    CHECK(!tap.release(left, 350, false));
    tap.press(left, 400, false, false);
    tap.observeKeyDown(right);
    tap.press(right, 420, false, true);
    CHECK(!tap.release(right, 440, false));
    tap.press(left, 500, false, false);
    CHECK(!tap.release(left, 1000, false));
    tap.press(left, 1010, false, false);
    tap.reset(); // Focus changed before the key went up.
    CHECK(!tap.release(left, 1020, false));
    tap.press(right, 1030, false, false);
    CHECK(tap.release(right, 1040, false));
    return true;
}

bool verifyFinalPipeReply() {
    const auto delayedRead = [](bool retainReply) {
        const std::wstring name = L"\\\\.\\pipe\\XMJD\\final-reply-" +
            std::to_wstring(GetCurrentProcessId()) + (retainReply ? L"-retained" : L"-discarded");
        HandleGuard server(xmjd::win::createServerPipe(true, name));
        CHECK(server.get() != INVALID_HANDLE_VALUE);
        xmjd::Response result;
        result.id.connectionEpoch = 1;
        result.id.sequence = 1;
        result.handled = true;
        const auto payload = xmjd::encodeResponse(result);
        CHECK(!payload.empty());
        std::thread writer;
        std::optional<std::string> received;
        bool written = false;
        DWORD beforeRead = WAIT_FAILED;
        {
            constexpr DWORD access = (FILE_GENERIC_READ | FILE_GENERIC_WRITE) & ~FILE_CREATE_PIPE_INSTANCE;
            HandleGuard client(CreateFileW(name.c_str(), access, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr));
            CHECK(client.get() != INVALID_HANDLE_VALUE);
            CHECK(xmjd::win::connectServerPipe(server.get(), nullptr));
            writer = std::thread([&] {
                written = retainReply
                    ? xmjd::win::writeFinalControlFrame(server.get(), payload, nullptr)
                    : xmjd::win::writeFrame(server.get(), payload, xmjd::kInputDeadlineMs);
                DisconnectNamedPipe(server.get());
            });
            // Reproduce the original race with a client that has not posted a read.
            beforeRead = WaitForSingleObject(writer.native_handle(), retainReply ? 150 : 1000);
            received = xmjd::win::readFrame(client.get(), 1000);
        }
        writer.join();
        CHECK(written);
        if (!retainReply) {
            CHECK(beforeRead == WAIT_OBJECT_0 && !received);
        } else {
            CHECK(beforeRead == WAIT_TIMEOUT && received && *received == payload);
        }
        return true;
    };
    return delayedRead(false) && delayedRead(true);
}

// Opt-in desktop integration test; the throwaway package is not Windows Search.
bool verifyAppContainerPeer() {
    struct TestProfile {
        std::wstring name = L"XmjdPipeTest-" + std::to_wstring(GetCurrentProcessId()) +
                            L"-" + std::to_wstring(GetTickCount64());
        PSID sid = nullptr;
        ~TestProfile() { if (sid) { DeleteAppContainerProfile(name.c_str()); FreeSid(sid); } }
    } profile;
    const HRESULT created = CreateAppContainerProfile(profile.name.c_str(), profile.name.c_str(),
        L"XMJD local pipe regression test", nullptr, 0, &profile.sid);
    if (FAILED(created)) std::cerr << "CreateAppContainerProfile: " << std::hex << created << std::dec << '\n';
    CHECK(SUCCEEDED(created) && profile.sid);
    struct ProfilePaths {
        LPWSTR sidText = nullptr;
        PWSTR directory = nullptr;
        ~ProfilePaths() { if (sidText) LocalFree(sidText); CoTaskMemFree(directory); }
    } paths;
    CHECK(ConvertSidToStringSidW(profile.sid, &paths.sidText));
    CHECK(SUCCEEDED(GetAppContainerFolderPath(paths.sidText, &paths.directory)) && paths.directory);
    const auto host = xmjd::win::modulePath();
    const auto probe = std::filesystem::path(paths.directory) / L"XMJDPeerProbe.exe";
    std::error_code ec;
    CHECK(std::filesystem::copy_file(host, probe, ec));
    const std::wstring name = L"\\\\.\\pipe\\XMJD\\" + profile.name;
    const auto run = [&](DWORD expectedExit) {
        HandleGuard pipe(xmjd::win::createServerPipe(true, name));
        CHECK(pipe.get() != INVALID_HANDLE_VALUE);
        struct Attributes {
            LPPROC_THREAD_ATTRIBUTE_LIST list = nullptr;
            ~Attributes() { if (list) DeleteProcThreadAttributeList(list); }
        };
        SIZE_T bytes = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
        std::vector<BYTE> data(bytes);
        auto* list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(data.data());
        CHECK(InitializeProcThreadAttributeList(list, 1, 0, &bytes));
        // Destroy the attribute list before releasing its backing storage.
        Attributes scoped{list};
        SECURITY_CAPABILITIES capabilities{profile.sid, nullptr, 0, 0};
        CHECK(UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES,
                                        &capabilities, sizeof(capabilities), nullptr, nullptr));
        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.lpAttributeList = list;
        PROCESS_INFORMATION process{};
        std::wstring command = L"\"" + probe.wstring() + L"\" --probe-appcontainer-peer \"" +
                               name + L"\" \"" + host.wstring() + L"\"";
        const BOOL started = CreateProcessW(probe.c_str(), command.data(), nullptr, nullptr, FALSE,
            EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW, nullptr, paths.directory,
            &startup.StartupInfo, &process);
        if (!started) std::cerr << "CreateProcessW: " << GetLastError() << '\n';
        CHECK(started);
        HandleGuard child(process.hProcess);
        HandleGuard thread(process.hThread);
        HandleGuard stop(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        CHECK(stop.get());
        std::thread server;
        if (expectedExit == 0) {
            server = std::thread([&] {
                if (!xmjd::win::connectServerPipe(pipe.get(), stop.get())) return;
                const auto frame = xmjd::win::readFrame(pipe.get(), 2000, stop.get());
                if (!frame) return;
                const auto client = xmjd::win::verifyClient(pipe.get());
                const auto request = xmjd::decodeRequest(*frame);
                if (!client || !client->appContainer || !request || request->operation != xmjd::Operation::Health) return;
                xmjd::Response response;
                response.id = request->id;
                response.handled = true;
                xmjd::win::writeFrame(pipe.get(), xmjd::encodeResponse(response), xmjd::kInputDeadlineMs);
            });
        }
        const DWORD wait = WaitForSingleObject(child.get(), 5000);
        SetEvent(stop.get());
        if (server.joinable()) server.join();
        if (wait != WAIT_OBJECT_0) {
            TerminateProcess(child.get(), 99);
            WaitForSingleObject(child.get(), 5000);
            return false;
        }
        DWORD exit = 0;
        CHECK(GetExitCodeProcess(child.get(), &exit));
        if (exit != expectedExit) std::cerr << "AppContainer probe exit: " << exit << '\n';
        return exit == expectedExit;
    };
    CHECK(run(10)); // The ordinary process DACL blocks authentication.
    const DWORD granted = xmjd::win::allowPipeServerIdentityQuery(profile.sid);
    if (granted != ERROR_SUCCESS) std::cerr << "allowPipeServerIdentityQuery: " << granted << '\n';
    CHECK(granted == ERROR_SUCCESS);
    CHECK(run(0));
    std::cout << "PASS: a low-IL AppContainer authenticated the desktop pipe and presented the shared candidate skin.\n";
    return true;
}

bool inspectInputProcesses() {
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    CHECK(snapshot != INVALID_HANDLE_VALUE);
    PROCESSENTRY32W entry{sizeof(entry)};
    bool found = false;
    if (Process32FirstW(snapshot, &entry)) do {
        if (_wcsicmp(entry.szExeFile, L"SearchHost.exe") != 0 &&
            _wcsicmp(entry.szExeFile, L"StartMenuExperienceHost.exe") != 0 &&
            _wcsicmp(entry.szExeFile, L"XMJDHost.exe") != 0) continue;
        found = true;
        const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
        HANDLE token = nullptr;
        if (!process || !OpenProcessToken(process, TOKEN_QUERY, &token)) {
            const DWORD error = GetLastError();
            if (process) CloseHandle(process);
            std::wcout << entry.szExeFile << L" pid=" << entry.th32ProcessID << L" token-error=" << error << L'\n';
            continue;
        }
        DWORD appContainer = 0, session = 0, bytes = sizeof(DWORD);
        const BOOL appRead = GetTokenInformation(token, TokenIsAppContainer, &appContainer, sizeof(appContainer), &bytes);
        const BOOL sessionRead = GetTokenInformation(token, TokenSessionId, &session, sizeof(session), &bytes);
        alignas(TOKEN_MANDATORY_LABEL) BYTE labelStorage[sizeof(TOKEN_MANDATORY_LABEL) + SECURITY_MAX_SID_SIZE]{};
        const BOOL labelRead = GetTokenInformation(token, TokenIntegrityLevel, labelStorage, sizeof(labelStorage), &bytes);
        DWORD integrity = 0;
        if (labelRead) {
            const auto* label = reinterpret_cast<const TOKEN_MANDATORY_LABEL*>(labelStorage);
            if (IsValidSid(label->Label.Sid) && *GetSidSubAuthorityCount(label->Label.Sid) > 0) {
                integrity = *GetSidSubAuthority(label->Label.Sid, *GetSidSubAuthorityCount(label->Label.Sid) - 1);
            }
        }
        std::wcout << entry.szExeFile << L" pid=" << entry.th32ProcessID
                   << L" appContainer=" << (appRead ? static_cast<int>(appContainer) : -1)
                   << L" restricted=" << (IsTokenRestricted(token) ? 1 : 0)
                   << L" integrity=0x" << std::hex << integrity << std::dec
                   << L" session=" << (sessionRead ? static_cast<int>(session) : -1) << L'\n';
        CloseHandle(token);
        CloseHandle(process);
    } while (Process32NextW(snapshot, &entry));
    CloseHandle(snapshot);
    return found;
}

bool verifyRegistration() {
    ComPtr<ITfInputProcessorProfileMgr> profiles;
    CHECK(CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER,
                            IID_PPV_ARGS(profiles.GetAddressOf())) == S_OK);
    constexpr GUID profileGuid = {0xc1fa1c93, 0xe62a, 0x4eda, {0xb2, 0x5a, 0xbc, 0x91, 0xc3, 0x3c, 0xca, 0x69}};
    TF_INPUTPROCESSORPROFILE profile{};
    CHECK(profiles->GetProfile(TF_PROFILETYPE_INPUTPROCESSOR, 0x0804, kTipClsid,
                              profileGuid, nullptr, &profile) == S_OK);
    std::cout << "Registered TIP capabilities=0x" << std::hex << profile.dwCaps
              << " flags=0x" << profile.dwFlags << std::dec << '\n';
    CHECK(profile.catid == GUID_TFCAT_TIP_KEYBOARD);
    CHECK((profile.dwCaps & TF_IPP_CAPS_UIELEMENTENABLED) != 0);
    // Check what TSF actually reports, rather than just the DLL's source list.
    // Desktop-only registrations used to pass installation tests yet could not
    // be selected in Windows app input fields.
    CHECK((profile.dwCaps & TF_IPP_CAPS_IMMERSIVESUPPORT) != 0);
    std::cout << "PASS: registered TIP supports UIElement and Windows apps.\n";
    return true;
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc == 4 && std::wstring_view(argv[1]) == L"--probe-appcontainer-peer") {
        xmjd::win::PipeClient client;
        if (client.connect(argv[3], argv[2])) {
            xmjd::Request health;
            health.id.connectionEpoch = client.epoch();
            health.id.sequence = 1;
            const auto response = client.transact(health);
            if (!response || !response->handled) return 15;
            if (!verifyCandidatePopup()) return 16;
            return verifyCandidatePopup(true) ? 0 : 17;
        }
        const std::string_view stage(client.connectFailureStage());
        if (stage == "peer-open-process" && client.connectFailureCode() == ERROR_ACCESS_DENIED) return 10;
        if (stage == "peer-open-token") return 11;
        if (stage == "peer-token-boundary") return 12;
        if (stage == "pipe-open") return 13;
        return 14;
    }
    if (argc == 2 && std::wstring_view(argv[1]) == L"--check-appcontainer-peer") {
        return verifyAppContainerPeer() ? 0 : 1;
    }
    if (argc == 2 && std::wstring_view(argv[1]) == L"--check-candidate-popup") {
        return verifyCandidatePopup() ? 0 : 1;
    }
    if (argc == 2 && std::wstring_view(argv[1]) == L"--check-candidate-composition") {
        return verifyCandidatePopup(true) ? 0 : 1;
    }
    if (argc == 2 && std::wstring_view(argv[1]) == L"--check-shift-tap") {
        return verifyShiftTap() ? 0 : 1;
    }
    if (argc == 2 && std::wstring_view(argv[1]) == L"--check-final-pipe-reply") {
        return verifyFinalPipeReply() ? 0 : 1;
    }
    if (argc != 2) {
        std::wcerr << L"用法: XMJDTipComTests <当前架构的 XMJDTip.dll 绝对路径> | --check-registration | "
                      L"--inspect-input-processes | --check-pipe-security | --check-appcontainer-peer | "
                      L"--check-final-pipe-reply | --check-shift-tap | --check-candidate-popup\n";
        return 2;
    }
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized)) {
        std::cerr << "COM 初始化失败: " << std::hex << initialized << '\n';
        return 1;
    }
    if (std::wstring_view(argv[1]) == L"--inspect-input-processes") {
        const bool passed = inspectInputProcesses();
        CoUninitialize();
        return passed ? 0 : 1;
    }
    if (std::wstring_view(argv[1]) == L"--check-registration") {
        const bool passed = verifyRegistration();
        CoUninitialize();
        return passed ? 0 : 1;
    }
    if (std::wstring_view(argv[1]) == L"--check-pipe-security") {
        const bool passed = verifyPipeSecurity();
        CoUninitialize();
        return passed ? 0 : 1;
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
