#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <olectl.h>
#include <msctf.h>
#include <ctfutb.h>
#include <oleauto.h>
#include <windowsx.h>
#include <wrl/client.h>
#include <algorithm>
#include <climits>
#include <atomic>
#include <filesystem>
#include <memory>
#include <new>
#include <string>
#include <utility>

#include "Tip.h"
#include "CandidateRenderer.h"

namespace xmjd::tip {
namespace {
const CLSID kTipClsid = {0x5f8f59f4, 0xe5a8, 0x4944, {0x96, 0x0b, 0x8a, 0x8a, 0xd3, 0xbe, 0x88, 0x57}};
const GUID kProfileGuid = {0xc1fa1c93, 0xe62a, 0x4eda, {0xb2, 0x5a, 0xbc, 0x91, 0xc3, 0x3c, 0xca, 0x69}};
const GUID kCandidateElementGuid = {0x7f58bce8, 0xd956, 0x4bdb, {0xb8, 0x34, 0x51, 0xdd, 0xc8, 0x5b, 0x46, 0x12}};
constexpr LANGID kLanguageId = 0x0804;
constexpr UINT kMaxPopupRows = 10;
constexpr UINT kMaskShift = 1u << 0;
constexpr UINT kMaskLock = 1u << 1;
constexpr UINT kMaskControl = 1u << 2;
constexpr UINT kMaskAlt = 1u << 3;
constexpr UINT kMaskSuper = 1u << 26;
constexpr UINT kMaskAltGr = 1u << 7;
constexpr DWORD kRegistryView = sizeof(void*) == 8 ? KEY_WOW64_64KEY : KEY_WOW64_32KEY;
// 与 register.ps1 契约一致：64 位 HKLM 视图的 REG_SZ 完整 Host 路径。
constexpr wchar_t kInstallMetadataKey[] = L"Software\\XMJD";
constexpr wchar_t kHostExecutableValue[] = L"HostExecutablePath";
constexpr UINT_PTR kEngineRetryTimerId = 0x584d4a44;
constexpr UINT kEngineRetryIntervalMs = 500;
constexpr ULONGLONG kHostLaunchIntervalMs = 2000;
constexpr wchar_t kPopupClassName[] = L"XMJD.TSF.CandidatePopup.v1";
const GUID kTipCategories[] = {GUID_TFCAT_TIP_KEYBOARD, GUID_TFCAT_TIPCAP_UIELEMENTENABLED};

using Microsoft::WRL::ComPtr;

HMODULE g_module = nullptr;
std::atomic<long> g_objectCount{0};
std::atomic<long> g_serverLocks{0};
SRWLOCK g_popupClassLock = SRWLOCK_INIT;
bool g_popupClassRegistered = false;
std::atomic<ULONGLONG> g_lastHostLaunch{0};

template <typename T>
void releaseInterface(T*& value) noexcept {
    if (T* previous = std::exchange(value, nullptr)) previous->Release();
}

std::wstring toWide(std::string_view value) {
    if (value.empty() || !validUtf8(value) || value.size() > INT_MAX) return {};
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (count <= 0) return {};
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), count) != count) return {};
    return result;
}

std::wstring formatCandidate(const CandidateValue& candidate) {
    std::wstring result;
    if (!candidate.label.empty()) {
        result.append(candidate.label);
        result.append(L"  ");
    }
    result.append(candidate.text);
    if (!candidate.comment.empty()) {
        result.append(L"  ");
        result.append(candidate.comment);
    }
    return result;
}

std::wstring guidString(REFGUID guid) {
    wchar_t text[40]{};
    if (StringFromGUID2(guid, text, static_cast<int>(std::size(text))) == 0) return {};
    return text;
}

std::wstring modulePath() {
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(g_module, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size()) return {};
    path.resize(length);
    return path;
}

bool writeRegString(const std::wstring& path, const wchar_t* name, const std::wstring& value) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, nullptr, 0, KEY_SET_VALUE | kRegistryView, nullptr, &key, nullptr) != ERROR_SUCCESS) return false;
    const DWORD bytes = static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
    const LONG result = RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()), bytes);
    RegCloseKey(key);
    return result == ERROR_SUCCESS;
}

HRESULT deleteOwnedTree(const wchar_t* parentPath, const std::wstring& childPath) noexcept {
    HKEY parent = nullptr;
    LONG result = RegOpenKeyExW(HKEY_LOCAL_MACHINE, parentPath, 0,
                                KEY_READ | KEY_WRITE | DELETE | kRegistryView, &parent);
    if (result == ERROR_FILE_NOT_FOUND || result == ERROR_PATH_NOT_FOUND) return S_OK;
    if (result != ERROR_SUCCESS) return HRESULT_FROM_WIN32(result);
    result = RegDeleteTreeW(parent, childPath.c_str());
    RegCloseKey(parent);
    return result == ERROR_SUCCESS || result == ERROR_FILE_NOT_FOUND || result == ERROR_PATH_NOT_FOUND
        ? S_OK : HRESULT_FROM_WIN32(result);
}

class RegistrationApartment {
public:
    RegistrationApartment() : result_(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) {}
    ~RegistrationApartment() { if (SUCCEEDED(result_)) CoUninitialize(); }
    HRESULT result() const { return result_ == RPC_E_CHANGED_MODE ? S_OK : result_; }
private:
    HRESULT result_;
};

HRESULT writeTipRegistration() {
    const std::wstring dll = modulePath();
    if (dll.empty()) return SELFREG_E_CLASS;
    RegistrationApartment apartment;
    if (FAILED(apartment.result())) return apartment.result();
    ComPtr<ITfInputProcessorProfiles> profiles;
    HRESULT hr = CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(profiles.GetAddressOf()));
    if (FAILED(hr)) return hr;
    ComPtr<ITfCategoryMgr> categories;
    hr = CoCreateInstance(CLSID_TF_CategoryMgr, nullptr, CLSCTX_INPROC_SERVER,
                          IID_PPV_ARGS(categories.GetAddressOf()));
    if (FAILED(hr)) return hr;

    const std::wstring clsid = guidString(kTipClsid);
    const std::wstring classPath = L"Software\\Classes\\CLSID\\" + clsid;
    const std::wstring inprocPath = classPath + L"\\InprocServer32";
    bool ok = writeRegString(classPath, nullptr, L"XMJD Text Service");
    ok = writeRegString(inprocPath, nullptr, dll) && ok;
    ok = writeRegString(inprocPath, L"ThreadingModel", L"Apartment") && ok;
    if (!ok) return SELFREG_E_CLASS;

    hr = profiles->Register(kTipClsid);
    if (FAILED(hr)) return hr;
    hr = profiles->AddLanguageProfile(kTipClsid, kLanguageId, kProfileGuid, L"XMJD", 4,
                                      dll.c_str(), static_cast<ULONG>(dll.size()), 0);
    if (FAILED(hr)) return hr;
    hr = profiles->EnableLanguageProfile(kTipClsid, kLanguageId, kProfileGuid, TRUE);
    if (FAILED(hr)) return hr;
    for (const GUID& category : kTipCategories) {
        hr = categories->RegisterCategory(kTipClsid, category, kTipClsid);
        if (FAILED(hr)) return hr;
    }
    return S_OK;
}

HRESULT enumeratorContains(IEnumGUID* enumerator, REFGUID expected, bool& present) {
    present = false;
    if (!enumerator) return E_UNEXPECTED;
    GUID value{};
    ULONG fetched = 0;
    HRESULT hr = S_OK;
    while ((hr = enumerator->Next(1, &value, &fetched)) == S_OK) {
        if (fetched != 1) return E_UNEXPECTED;
        if (value == expected) {
            present = true;
            return S_OK;
        }
    }
    return hr == S_FALSE ? S_OK : (FAILED(hr) ? hr : E_UNEXPECTED);
}

HRESULT categoryPresent(ITfCategoryMgr* categories, REFGUID category, bool& present) {
    ComPtr<IEnumGUID> enumerator;
    const HRESULT hr = categories->EnumCategoriesInItem(kTipClsid, enumerator.GetAddressOf());
    return FAILED(hr) ? hr : enumeratorContains(enumerator.Get(), category, present);
}

HRESULT removeCategory(ITfCategoryMgr* categories, REFGUID category) {
    bool present = false;
    const HRESULT hr = categoryPresent(categories, category, present);
    if (FAILED(hr) || !present) return hr;
    // TSF caches category enumeration within the registrar process, including
    // newly created managers. Confirm persisted removal after profile cleanup
    // instead of interpreting an in-process snapshot as a failure.
    return categories->UnregisterCategory(kTipClsid, category, kTipClsid);
}

HRESULT removeTipRegistration() {
    RegistrationApartment apartment;
    if (FAILED(apartment.result())) return apartment.result();
    HRESULT result = S_OK;
    ComPtr<ITfCategoryMgr> categories;
    HRESULT hr = CoCreateInstance(CLSID_TF_CategoryMgr, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(categories.GetAddressOf()));
    if (SUCCEEDED(hr)) {
        for (const GUID& category : kTipCategories) {
            hr = removeCategory(categories.Get(), category);
            if (FAILED(hr) && SUCCEEDED(result)) result = hr;
        }
    } else {
        result = hr;
    }
    ComPtr<ITfInputProcessorProfiles> profiles;
    hr = CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER,
                          IID_PPV_ARGS(profiles.GetAddressOf()));
    if (SUCCEEDED(hr)) {
        ComPtr<IEnumGUID> enumerator;
        bool present = false;
        hr = profiles->EnumInputProcessorInfo(enumerator.GetAddressOf());
        if (SUCCEEDED(hr)) hr = enumeratorContains(enumerator.Get(), kTipClsid, present);
        if (SUCCEEDED(hr) && present) {
            const HRESULT removed = profiles->Unregister(kTipClsid);
            enumerator.Reset();
            hr = profiles->EnumInputProcessorInfo(enumerator.GetAddressOf());
            if (SUCCEEDED(hr)) hr = enumeratorContains(enumerator.Get(), kTipClsid, present);
            if (SUCCEEDED(hr) && present) hr = FAILED(removed) ? removed : E_FAIL;
        }
        if (FAILED(hr) && SUCCEEDED(result)) result = hr;
    } else if (SUCCEEDED(result)) {
        result = hr;
    }
    // The product's machine TSF registration lives under this shared key.
    // Registry reads observe persisted state, unlike the per-process category
    // enumeration cache. Check both views and never mask access/query errors.
    const std::wstring tipPath = L"Software\\Microsoft\\CTF\\TIP\\" + guidString(kTipClsid);
    for (DWORD view : {static_cast<DWORD>(KEY_WOW64_64KEY), static_cast<DWORD>(KEY_WOW64_32KEY)}) {
        HKEY key = nullptr;
        const LONG opened = RegOpenKeyExW(HKEY_LOCAL_MACHINE, tipPath.c_str(), 0, KEY_READ | view, &key);
        if (opened == ERROR_SUCCESS) RegCloseKey(key);
        hr = opened == ERROR_FILE_NOT_FOUND || opened == ERROR_PATH_NOT_FOUND
            ? S_OK : (opened == ERROR_SUCCESS ? E_FAIL : HRESULT_FROM_WIN32(opened));
        if (FAILED(hr) && SUCCEEDED(result)) result = hr;
    }
    hr = deleteOwnedTree(L"Software\\Classes\\CLSID", guidString(kTipClsid));
    if (FAILED(hr) && SUCCEEDED(result)) result = hr;
    return result;
}

HRESULT requestEdit(ITfContext* context, TfClientId clientId, ITfEditSession* edit, DWORD flags, HRESULT* editResult) {
    if (!context || !edit || !editResult) return E_INVALIDARG;
    *editResult = E_FAIL;
    const HRESULT requestResult = context->RequestEditSession(clientId, edit, flags, editResult);
    if (FAILED(requestResult)) return requestResult;
    return *editResult;
}

} // namespace

XMJDTip::XMJDTip() { ++g_objectCount; }
XMJDTip::~XMJDTip() {
    deactivate();
    --g_objectCount;
}

HRESULT STDMETHODCALLTYPE XMJDTip::QueryInterface(REFIID riid, void** object) {
    if (!object) return E_POINTER;
    *object = nullptr;
    if (riid == IID_IUnknown || riid == IID_ITfTextInputProcessorEx) *object = static_cast<ITfTextInputProcessorEx*>(this);
    else if (riid == IID_ITfTextInputProcessor) *object = static_cast<ITfTextInputProcessor*>(this);
    else if (riid == IID_ITfKeyEventSink) *object = static_cast<ITfKeyEventSink*>(this);
    else if (riid == IID_ITfThreadMgrEventSink) *object = static_cast<ITfThreadMgrEventSink*>(this);
    else if (riid == IID_ITfCompositionSink) *object = static_cast<ITfCompositionSink*>(this);
    else if (riid == IID_ITfInputProcessorProfileActivationSink) *object = static_cast<ITfInputProcessorProfileActivationSink*>(this);
    else return E_NOINTERFACE;
    AddRef();
    return S_OK;
}
ULONG STDMETHODCALLTYPE XMJDTip::AddRef() { return ++refs_; }
ULONG STDMETHODCALLTYPE XMJDTip::Release() {
    const ULONG remaining = --refs_;
    if (remaining == 0) delete this;
    return remaining;
}

HRESULT STDMETHODCALLTYPE XMJDTip::Activate(ITfThreadMgr* manager, TfClientId clientId) {
    return ActivateEx(manager, clientId, 0);
}
HRESULT STDMETHODCALLTYPE XMJDTip::ActivateEx(ITfThreadMgr* manager, TfClientId clientId, DWORD flags) {
    if (!manager) return E_INVALIDARG;
    if (activated_) return TF_E_ALREADY_EXISTS;
    try {
        threadManager_ = manager;
        threadManager_->AddRef();
        clientId_ = clientId;
        activationFlags_ = flags;
        uiLess_ = false;
        ComPtr<ITfThreadMgrEx> extendedThreadManager;
        if (SUCCEEDED(threadManager_->QueryInterface(IID_PPV_ARGS(extendedThreadManager.GetAddressOf())))) {
            DWORD activeFlags = 0;
            if (SUCCEEDED(extendedThreadManager->GetActiveFlags(&activeFlags))) {
                uiLess_ = (activeFlags & TF_TMF_UIELEMENTENABLEDONLY) != 0;
            }
        }
        HRESULT hr = threadManager_->QueryInterface(IID_PPV_ARGS(&keystrokeManager_));
        if (FAILED(hr)) { deactivate(); return hr; }
        hr = threadManager_->QueryInterface(IID_PPV_ARGS(&uiElementManager_));
        if (FAILED(hr)) { deactivate(); return hr; }
        hr = threadManager_->QueryInterface(IID_PPV_ARGS(&threadSource_));
        if (FAILED(hr)) { deactivate(); return hr; }
        hr = keystrokeManager_->AdviseKeyEventSink(clientId_, this, TRUE);
        if (FAILED(hr)) { deactivate(); return hr; }
        keySinkAdvised_ = true;
        hr = threadSource_->AdviseSink(IID_ITfThreadMgrEventSink,
                                       static_cast<ITfThreadMgrEventSink*>(this), &threadSinkCookie_);
        if (FAILED(hr)) { deactivate(); return hr; }
        hr = threadSource_->AdviseSink(IID_ITfInputProcessorProfileActivationSink,
                                       static_cast<ITfInputProcessorProfileActivationSink*>(this), &profileSinkCookie_);
        if (FAILED(hr)) { deactivate(); return hr; }
        activated_ = true;
        foreground_ = true;
        createCandidateWindow();
        ITfContext* focusedContext = nullptr;
        ITfDocumentMgr* focusedDocument = nullptr;
        if (SUCCEEDED(getFocusedContext(&focusedContext, &focusedDocument))) {
            const bool contextReady = ensureContext(focusedContext);
            releaseInterface(focusedContext);
            releaseInterface(focusedDocument);
            if (contextReady) scheduleEngineRetry();
        }
        return S_OK;
    } catch (...) {
        deactivate();
        return E_UNEXPECTED;
    }
}
HRESULT STDMETHODCALLTYPE XMJDTip::Deactivate() {
    deactivate();
    return S_OK;
}

void XMJDTip::deactivate() noexcept {
    foreground_ = false;
    cancelEngineRetry();
    resetForFocusChange();
    if (profileSinkCookie_ != TF_INVALID_COOKIE && threadSource_) {
        threadSource_->UnadviseSink(profileSinkCookie_);
        profileSinkCookie_ = TF_INVALID_COOKIE;
    }
    if (threadSinkCookie_ != TF_INVALID_COOKIE && threadSource_) {
        threadSource_->UnadviseSink(threadSinkCookie_);
        threadSinkCookie_ = TF_INVALID_COOKIE;
    }
    if (candidateWindow_) {
        const HWND window = std::exchange(candidateWindow_, nullptr);
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        DestroyWindow(window);
    }
    if (keySinkAdvised_ && keystrokeManager_) keystrokeManager_->UnadviseKeyEventSink(clientId_);
    keySinkAdvised_ = false;
    activated_ = false;
    releaseInterface(threadSource_);
    releaseInterface(uiElementManager_);
    releaseInterface(keystrokeManager_);
    releaseInterface(threadManager_);
    clientId_ = TF_CLIENTID_NULL;
    acknowledgedRaw_.clear();
    contextUnavailable_ = false;
}

HRESULT XMJDTip::getFocusedContext(ITfContext** context, ITfDocumentMgr** documentManager) const {
    if (!context || !documentManager) return E_POINTER;
    *context = nullptr;
    *documentManager = nullptr;
    ComPtr<ITfThreadMgr> manager = threadManager_;
    if (!manager) return TF_E_DISCONNECTED;
    HRESULT hr = manager->GetFocus(documentManager);
    if (FAILED(hr) || !*documentManager) return FAILED(hr) ? hr : TF_E_DISCONNECTED;
    hr = (*documentManager)->GetTop(context);
    if (FAILED(hr) || !*context) {
        releaseInterface(*documentManager);
        return FAILED(hr) ? hr : TF_E_DISCONNECTED;
    }
    return S_OK;
}

bool XMJDTip::isCurrentContext(ITfContext* context) const {
    if (!context || !foreground_ || resettingContext_) return false;
    ITfContext* focused = nullptr;
    ITfDocumentMgr* document = nullptr;
    if (FAILED(getFocusedContext(&focused, &document))) return false;
    IUnknown* expected = nullptr;
    IUnknown* actual = nullptr;
    const bool same = SUCCEEDED(context->QueryInterface(IID_PPV_ARGS(&expected))) &&
                      SUCCEEDED(focused->QueryInterface(IID_PPV_ARGS(&actual))) && expected == actual;
    releaseInterface(expected);
    releaseInterface(actual);
    releaseInterface(focused);
    releaseInterface(document);
    return same && activated_ && foreground_ && !resettingContext_;
}

bool XMJDTip::isValidEdit(const EditSession* edit, bool requireFocus) const {
    if (!edit || !context_ || edit->context_ != context_ || edit->epoch_ != contextEpoch_) return false;
    if (requireFocus && (!activated_ || !isCurrentContext(edit->context_))) return false;
    return edit->context_ == context_ && edit->epoch_ == contextEpoch_;
}

HRESULT XMJDTip::requestEdit(ITfContext* context, EditSession* edit, DWORD flags, HRESULT* sessionResult) {
    if (!edit || edit->context_ != context) return E_INVALIDARG;
    return xmjd::tip::requestEdit(context, clientId_, edit, flags, sessionResult);
}

bool XMJDTip::ensureContext(ITfContext* context) {
    if (!context || resettingContext_) return false;
    IUnknown* incoming = nullptr;
    IUnknown* current = nullptr;
    if (FAILED(context->QueryInterface(IID_PPV_ARGS(&incoming)))) return false;
    bool same = false;
    if (context_ && SUCCEEDED(context_->QueryInterface(IID_PPV_ARGS(&current)))) same = current == incoming;
    releaseInterface(incoming);
    releaseInterface(current);
    if (same && !compositionTerminated_) return true;

    // reset 可能释放与传入参数相同的上下文，先保留调用方的目标。
    ComPtr<ITfContext> incomingContext = context;
    resetForFocusChange();
    if (!activated_ || !isCurrentContext(incomingContext.Get())) return false;
    ComPtr<ITfDocumentMgr> incomingDocument;
    if (FAILED(incomingContext->GetDocumentMgr(incomingDocument.GetAddressOf())) || !incomingDocument ||
        !activated_ || !isCurrentContext(incomingContext.Get())) return false;
    context_ = incomingContext.Detach();
    documentManager_ = incomingDocument.Detach();
    scheduleEngineRetry();
    return true;
}

void XMJDTip::invalidateSession(bool closePipe) noexcept {
    hasEngineSession_ = false;
    sessionId_ = 0;
    if (closePipe) pipe_.close();
}

bool XMJDTip::transact(Operation operation, Request& request, Response& response) {
    request.id.version = kProtocolVersion;
    request.id.connectionEpoch = pipe_.epoch();
    request.id.sessionId = sessionId_;
    request.id.contextEpoch = contextEpoch_;
    request.id.sequence = ++sequence_;
    request.operation = operation;
    try {
        const auto result = pipe_.transact(request, kInputDeadlineMs);
        if (!result || result->id.contextEpoch != request.id.contextEpoch ||
            result->id.connectionEpoch != request.id.connectionEpoch || result->id.sequence != request.id.sequence) return false;
        if (operation != Operation::CreateSession && result->id.sessionId != request.id.sessionId) return false;
        response = *result;
        if (operation == Operation::CreateSession && response.status == Status::Ok && response.handled) {
            sessionId_ = response.id.sessionId;
            hasEngineSession_ = sessionId_ != 0;
        }
        return true;
    } catch (...) {
        return false;
    }
}

bool XMJDTip::createEngineSession() {
    if (hasEngineSession_) return true;
    if (!activated_ || !context_ || contextEpoch_ == 0 || !foreground_ || resettingContext_ ||
        contextUnavailable_ || composition_ || !isCurrentContext(context_)) return false;
    if (!pipe_.connected()) {
        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kInstallMetadataKey, 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS) return false;
        DWORD type = 0;
        DWORD bytes = 0;
        LONG query = RegQueryValueExW(key, kHostExecutableValue, nullptr, &type, nullptr, &bytes);
        if (query != ERROR_SUCCESS || type != REG_SZ || bytes < 2 * sizeof(wchar_t) ||
            bytes > 32768 * sizeof(wchar_t) || bytes % sizeof(wchar_t) != 0) {
            RegCloseKey(key);
            return false;
        }
        std::wstring executable(bytes / sizeof(wchar_t), L'\0');
        query = RegQueryValueExW(key, kHostExecutableValue, nullptr, &type, reinterpret_cast<BYTE*>(executable.data()), &bytes);
        RegCloseKey(key);
        if (query != ERROR_SUCCESS || type != REG_SZ || bytes < 2 * sizeof(wchar_t) ||
            bytes % sizeof(wchar_t) != 0) return false;
        executable.resize(bytes / sizeof(wchar_t));
        if (executable.back() != L'\0') return false;
        executable.pop_back();
        if (executable.find_first_of(L"\"\r\n") != std::wstring::npos || executable.find(L'\0') != std::wstring::npos) return false;
        const std::filesystem::path hostPath(executable);
        std::error_code error;
        if (!hostPath.is_absolute() || !std::filesystem::is_regular_file(hostPath, error)) return false;
        if (!pipe_.connect(hostPath)) {
            // Never inherit an elevated or restricted application's token.
            HANDLE token = nullptr;
            if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
            TOKEN_ELEVATION elevation{};
            DWORD returned = 0;
            const bool ordinary = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &returned) &&
                                  !elevation.TokenIsElevated && !IsTokenRestricted(token);
            CloseHandle(token);
            if (!ordinary) return false;
            const ULONGLONG now = GetTickCount64();
            ULONGLONG previous = g_lastHostLaunch.load();
            if (previous != 0 && now - previous < kHostLaunchIntervalMs) return false;
            if (!g_lastHostLaunch.compare_exchange_strong(previous, now == 0 ? 1 : now)) return false;
            // 显式指定映像；引号中的 argv[0] 不得通过命令行重新选择程序。
            std::wstring commandLine = L"\"" + executable + L"\"";
            STARTUPINFOW startup{sizeof(startup)};
            PROCESS_INFORMATION process{};
            const std::wstring directory = hostPath.parent_path().wstring();
            if (CreateProcessW(executable.c_str(), commandLine.data(), nullptr, nullptr, FALSE,
                               CREATE_NO_WINDOW, nullptr, directory.c_str(), &startup, &process)) {
                CloseHandle(process.hThread);
                CloseHandle(process.hProcess);
            }
            return false;
        }
    }
    Request request;
    Response response;
    if (!transact(Operation::CreateSession, request, response) || response.status != Status::Ok || !response.handled) {
        pipe_.close();
        return false;
    }
    return hasEngineSession_;
}

bool XMJDTip::recoverComposition(EditSession* edit, TfEditCookie cookie) {
    if (!isValidEdit(edit)) return false;
    if (composition_) {
        // 只替换原 composition，绝不把失败前的编码插入新的 selection。
        if (!acknowledgedRaw_.empty() &&
            !setCompositionText(edit, cookie, acknowledgedRaw_, static_cast<UINT>(acknowledgedRaw_.size()))) return false;
        if (!endComposition(edit, cookie, acknowledgedRaw_.empty())) return false;
    }
    if (!isValidEdit(edit)) return false;
    acknowledgedRaw_.clear();
    contextUnavailable_ = false;
    compositionTerminated_ = false;
    if (++contextEpoch_ == 0) ++contextEpoch_;
    return true;
}

void XMJDTip::failEngineSession(EditSession* edit, TfEditCookie cookie) {
    if (!isValidEdit(edit, false)) return;
    invalidateSession(true);
    contextUnavailable_ = true;
    candidateRevision_ = 0;
    closeCandidateUi();
    if (!isValidEdit(edit, false)) return;
    recoverComposition(edit, cookie);
    scheduleEngineRetry();
}

void XMJDTip::retryEngineSession() {
    if (!activated_ || resettingContext_) return;
    if (!foreground_ || !context_) { cancelEngineRetry(); return; }
    if (!isCurrentContext(context_)) { resetForFocusChange(); return; }
    if (contextUnavailable_) {
        EditSession* edit = new (std::nothrow) EditSession(this, EditAction::RecoverComposition, 0, 0, 0, 0, contextEpoch_);
        if (!edit) return;
        HRESULT result = E_FAIL;
        requestEdit(context_, edit, TF_ES_ASYNCDONTCARE | TF_ES_READWRITE, &result);
        edit->Release();
        return;
    }
    if (createEngineSession()) cancelEngineRetry();
}

void XMJDTip::scheduleEngineRetry() {
    if (!activated_ || !foreground_ || !context_ || engineRetryScheduled_) return;
    if (createCandidateWindow()) {
        engineRetryScheduled_ = SetTimer(candidateWindow_, kEngineRetryTimerId, kEngineRetryIntervalMs, nullptr) != 0;
    }
}

void XMJDTip::cancelEngineRetry() noexcept {
    if (candidateWindow_) KillTimer(candidateWindow_, kEngineRetryTimerId);
    engineRetryScheduled_ = false;
}

bool XMJDTip::startComposition(EditSession* edit, TfEditCookie cookie) {
    if (!isValidEdit(edit)) return false;
    if (composition_) return true;
    ComPtr<ITfContext> target = edit->context_;
    TF_SELECTION selection{};
    ULONG fetched = 0;
    HRESULT hr = target->GetSelection(cookie, TF_DEFAULT_SELECTION, 1, &selection, &fetched);
    if (FAILED(hr) || fetched != 1 || !selection.range) return false;
    ITfContextComposition* contextComposition = nullptr;
    hr = target->QueryInterface(IID_PPV_ARGS(&contextComposition));
    if (FAILED(hr) || !contextComposition || !isValidEdit(edit)) {
        releaseInterface(contextComposition);
        selection.range->Release();
        return false;
    }
    ITfComposition* started = nullptr;
    hr = contextComposition->StartComposition(cookie, selection.range, this, &started);
    contextComposition->Release();
    selection.range->Release();
    if (FAILED(hr) || !started) return false;
    if (!isValidEdit(edit)) {
        started->EndComposition(cookie);
        started->Release();
        return false;
    }
    composition_ = started;
    return true;
}

bool XMJDTip::setCompositionText(EditSession* edit, TfEditCookie cookie, std::wstring_view text, UINT caret) {
    if (!isValidEdit(edit) || (!composition_ && !startComposition(edit, cookie))) return false;
    ComPtr<ITfContext> target = edit->context_;
    ComPtr<ITfComposition> active = composition_;
    ITfRange* range = nullptr;
    if (FAILED(active->GetRange(&range)) || !range) return false;
    const HRESULT textResult = isValidEdit(edit)
        ? range->SetText(cookie, 0, text.empty() ? L"" : text.data(), static_cast<LONG>(text.size())) : TF_E_DISCONNECTED;
    range->Release();
    if (FAILED(textResult) || !isValidEdit(edit) || active.Get() != composition_ ||
        FAILED(active->GetRange(&range)) || !range) return false;
    ITfRange* caretRange = nullptr;
    HRESULT hr = range->Clone(&caretRange);
    range->Release();
    if (FAILED(hr) || !caretRange) return false;
    hr = caretRange->Collapse(cookie, TF_ANCHOR_START);
    LONG shifted = 0;
    const LONG boundedCaret = static_cast<LONG>(std::min<std::size_t>(caret, text.size()));
    if (SUCCEEDED(hr)) hr = caretRange->ShiftEnd(cookie, boundedCaret, &shifted, nullptr);
    if (SUCCEEDED(hr) && shifted != boundedCaret) hr = E_FAIL;
    if (SUCCEEDED(hr)) hr = caretRange->Collapse(cookie, TF_ANCHOR_END);
    if (SUCCEEDED(hr)) {
        TF_SELECTION selection{};
        selection.range = caretRange;
        selection.style.ase = TF_AE_NONE;
        selection.style.fInterimChar = FALSE;
        hr = isValidEdit(edit) ? target->SetSelection(cookie, 1, &selection) : TF_E_DISCONNECTED;
    }
    caretRange->Release();
    return SUCCEEDED(hr);
}

bool XMJDTip::commitText(EditSession* edit, TfEditCookie cookie, std::wstring_view text) {
    if (!isValidEdit(edit)) return false;
    if (text.empty()) return true;
    ComPtr<ITfContext> target = edit->context_;
    const wchar_t* value = text.data();
    if (composition_) {
        ComPtr<ITfComposition> active = composition_;
        ITfRange* range = nullptr;
        if (FAILED(active->GetRange(&range)) || !range) return false;
        const HRESULT hr = isValidEdit(edit)
            ? range->SetText(cookie, 0, value, static_cast<LONG>(text.size())) : TF_E_DISCONNECTED;
        range->Release();
        if (FAILED(hr)) return false;
        if (!isValidEdit(edit) || active.Get() != composition_) return true;
        return endComposition(edit, cookie, false);
    }
    ComPtr<ITfInsertAtSelection> insertion;
    HRESULT hr = target->QueryInterface(IID_PPV_ARGS(insertion.GetAddressOf()));
    if (FAILED(hr) || !isValidEdit(edit)) return false;
    ComPtr<ITfRange> inserted;
    hr = insertion->InsertTextAtSelection(cookie, 0, value, static_cast<LONG>(text.size()), inserted.GetAddressOf());
    if (FAILED(hr) || !inserted) return false;
    // 插入已经成功，光标调整失败不得触发重放。
    if (isValidEdit(edit) && SUCCEEDED(inserted->Collapse(cookie, TF_ANCHOR_END)) && isValidEdit(edit)) {
        TF_SELECTION selection{};
        selection.range = inserted.Get();
        selection.style.ase = TF_AE_NONE;
        target->SetSelection(cookie, 1, &selection);
    }
    return true;
}

bool XMJDTip::endComposition(EditSession* edit, TfEditCookie cookie, bool eraseText) {
    const bool requireFocus = edit && edit->action_ != EditAction::CancelComposition;
    if (!isValidEdit(edit, requireFocus)) return false;
    if (!composition_) return true;
    ComPtr<ITfComposition> ending = composition_;
    if (eraseText) {
        ITfRange* range = nullptr;
        if (FAILED(ending->GetRange(&range)) || !range) return false;
        const HRESULT textResult = isValidEdit(edit, requireFocus)
            ? range->SetText(cookie, 0, L"", 0) : TF_E_DISCONNECTED;
        range->Release();
        if (FAILED(textResult)) return false;
    }
    if (!isValidEdit(edit, requireFocus) || ending.Get() != composition_) return false;
    // 主动结束时先摘下，避免同步 OnCompositionTerminated 把正常提交误判为外部终止。
    ITfComposition* owned = std::exchange(composition_, nullptr);
    const HRESULT hr = ending->EndComposition(cookie);
    if (FAILED(hr) && isValidEdit(edit, requireFocus) && !composition_) composition_ = std::exchange(owned, nullptr);
    releaseInterface(owned);
    return SUCCEEDED(hr);
}

bool XMJDTip::applySnapshot(EditSession* edit, TfEditCookie cookie, const Snapshot& snapshot) {
    if (!isValidEdit(edit)) return false;
    if (!snapshot.commitText.empty()) {
        const std::u16string utf16 = utf8ToUtf16(snapshot.commitText);
        if (!validUtf8(snapshot.commitText) || !commitText(edit, cookie, std::wstring_view(reinterpret_cast<const wchar_t*>(utf16.data()), utf16.size()))) return false;
    }
    if (!isValidEdit(edit)) return false;
    if (!snapshot.preedit.empty()) {
        const std::u16string utf16 = utf8ToUtf16(snapshot.preedit);
        const UINT caret = static_cast<UINT>(utf16Caret(snapshot.preedit, snapshot.caretUtf8Offset));
        if (!validUtf8(snapshot.preedit) || !setCompositionText(edit, cookie, std::wstring_view(reinterpret_cast<const wchar_t*>(utf16.data()), utf16.size()), caret)) return false;
    } else if (composition_ && !endComposition(edit, cookie, true)) {
        return false;
    }
    if (!isValidEdit(edit)) return false;
    acknowledgedRaw_ = toWide(snapshot.rawInput);
    candidateRevision_ = snapshot.revision;
    updateCandidateUi(snapshot, cookie);
    return true;
}

void XMJDTip::updateCandidateUi(const Snapshot& snapshot, TfEditCookie cookie) {
    if (snapshot.candidates.empty()) {
        closeCandidateUi();
        return;
    }
    if (!candidateElement_) candidateElement_ = new (std::nothrow) CandidateElement(this, context_);
    if (!candidateElement_ || !uiElementManager_) return;
    ComPtr<CandidateElement> element = candidateElement_;
    ComPtr<ITfUIElementMgr> manager = uiElementManager_;
    const std::uint64_t epoch = contextEpoch_;
    element->update(snapshot);
    if (uiElementId_ == TF_INVALID_UIELEMENTID) {
        BOOL show = uiLess_ ? FALSE : TRUE;
        DWORD id = TF_INVALID_UIELEMENTID;
        const HRESULT hr = manager->BeginUIElement(element.Get(), &show, &id);
        if (FAILED(hr)) { showCandidateWindow(false); return; }
        if (element.Get() != candidateElement_ || epoch != contextEpoch_) {
            if (id != TF_INVALID_UIELEMENTID) manager->EndUIElement(id);
            return;
        }
        uiElementId_ = id;
        uiElementShow_ = show != FALSE && !uiLess_;
        element->Show(uiElementShow_ ? TRUE : FALSE);
    }
    if (element.Get() != candidateElement_ || epoch != contextEpoch_) return;
    if (uiElementId_ != TF_INVALID_UIELEMENTID) manager->UpdateUIElement(uiElementId_);
    if (element.Get() != candidateElement_ || epoch != contextEpoch_) return;
    if (uiElementShow_ && element->isShown()) {
        if (!positionCandidateWindow(cookie)) showCandidateWindow(false);
    } else {
        showCandidateWindow(false);
    }
}

void XMJDTip::closeCandidateUi() noexcept {
    uiElementShow_ = false;
    showCandidateWindow(false);
    CandidateElement* element = std::exchange(candidateElement_, nullptr);
    const DWORD id = std::exchange(uiElementId_, TF_INVALID_UIELEMENTID);
    if (element) element->detach();
    if (id != TF_INVALID_UIELEMENTID && uiElementManager_) uiElementManager_->EndUIElement(id);
    if (element) element->Release();
}

bool XMJDTip::positionCandidateWindow(TfEditCookie cookie) {
    if (!candidateWindow_ || !context_ || !composition_ || !candidateElement_ || !uiElementShow_ || uiLess_) return false;
    const std::uint64_t epoch = contextEpoch_;
    ComPtr<ITfContext> context = context_;
    ComPtr<ITfComposition> composition = composition_;
    ComPtr<CandidateElement> element = candidateElement_;
    ITfRange* range = nullptr;
    ITfContextView* view = nullptr;
    if (FAILED(composition->GetRange(&range)) || !range || FAILED(context->GetActiveView(&view)) || !view) {
        releaseInterface(range);
        releaseInterface(view);
        return false;
    }
    RECT caret{};
    BOOL clipped = FALSE;
    HRESULT hr = view->GetTextExt(cookie, range, &caret, &clipped);
    range->Release();
    view->Release();
    if (FAILED(hr) || epoch != contextEpoch_ || context.Get() != context_ || element.Get() != candidateElement_ ||
        !uiElementShow_ || !element->isShown() || !isCurrentContext(context.Get()) || epoch != contextEpoch_) return false;
    HMONITOR monitor = MonitorFromRect(&caret, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{sizeof(info)};
    if (!monitor || !GetMonitorInfoW(monitor, &info)) return false;
    const int dpi = static_cast<int>(GetDpiForWindow(candidateWindow_));
    if (!candidateRenderer_) candidateRenderer_ = std::make_unique<CandidateRenderer>();
    if (!candidateRenderer_) return false;
    const CandidateSnapshot& snapshot = element->snapshot();
    if (!candidateRenderer_->prepare(snapshot.values, snapshot.selected, kMaxPopupRows, dpi == 0 ? 96 : dpi)) {
        return false;
    }
    const xmjd::CandidateLayout& layout = candidateRenderer_->layout();
    const int width = layout.width;
    const int height = layout.height;
    LONG x = caret.left;
    LONG y = caret.bottom;
    if (y + height > info.rcWork.bottom) y = caret.top - height;
    x = std::max(info.rcWork.left, std::min(x, info.rcWork.right - width));
    y = std::max(info.rcWork.top, std::min(y, info.rcWork.bottom - height));
    if (!SetWindowPos(candidateWindow_, HWND_TOPMOST, x, y, width, height, SWP_NOACTIVATE)) return false;
    return candidateRenderer_->paint(candidateWindow_, dpi == 0 ? 96 : dpi, x, y);
}

bool XMJDTip::createCandidateWindow() {
    if (candidateWindow_) return true;
    AcquireSRWLockExclusive(&g_popupClassLock);
    if (!g_popupClassRegistered) {
        WNDCLASSEXW cls{sizeof(cls)};
        cls.lpfnWndProc = popupWndProc;
        cls.hInstance = g_module;
        cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        cls.hbrBackground = nullptr;
        cls.lpszClassName = kPopupClassName;
        g_popupClassRegistered = RegisterClassExW(&cls) != 0;
    }
    const bool registered = g_popupClassRegistered;
    ReleaseSRWLockExclusive(&g_popupClassLock);
    if (!registered) return false;
    candidateWindow_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_LAYERED,
                                        kPopupClassName, L"", WS_POPUP,
                                        0, 0, 1, 1, nullptr, nullptr, g_module, this);
    return candidateWindow_ != nullptr;
}

bool XMJDTip::showCandidateWindow(bool show) {
    if (!candidateWindow_) return !show;
    show = show && !uiLess_ && uiElementShow_;
    ShowWindow(candidateWindow_, show ? SW_SHOWNOACTIVATE : SW_HIDE);
    if (show) InvalidateRect(candidateWindow_, nullptr, TRUE);
    return true;
}

void XMJDTip::candidateVisibilityChanged(bool shown) {
    if (!shown || !uiElementShow_) {
        showCandidateWindow(false);
        return;
    }
    if (!context_ || contextUnavailable_ || !candidateElement_ || candidateElement_->snapshot().values.empty() ||
        candidateElement_->snapshot().epoch != contextEpoch_ || !isCurrentContext(context_)) return;
    EditSession* edit = new (std::nothrow) EditSession(this, EditAction::UpdateCandidatePosition, 0, 0, 0, candidateRevision_, contextEpoch_);
    if (!edit) return;
    HRESULT result = E_FAIL;
    requestEdit(context_, edit, TF_ES_ASYNC | TF_ES_READ, &result);
    edit->Release();
}

void XMJDTip::candidateClicked(UINT index, std::uint64_t revision) {
    if (!context_ || contextUnavailable_ || revision != candidateRevision_ ||
        !candidateElement_ || candidateElement_->snapshot().epoch != contextEpoch_ || !isCurrentContext(context_)) return;
    EditSession* edit = new (std::nothrow) EditSession(this, EditAction::SelectCandidate, 0, 0, index, revision, contextEpoch_);
    if (!edit) return;
    HRESULT result = E_FAIL;
    requestEdit(context_, edit, TF_ES_ASYNC | TF_ES_READWRITE, &result);
    edit->Release();
}

bool XMJDTip::processCandidateSelection(EditSession* edit, TfEditCookie cookie) {
    if (!isValidEdit(edit) || contextUnavailable_ || edit->revision_ != candidateRevision_ ||
        !candidateElement_ || candidateElement_->snapshot().epoch != contextEpoch_ || !hasEngineSession_) return false;
    Request request;
    request.candidateIndex = edit->candidateIndex_;
    request.snapshotRevision = edit->revision_;
    Response response;
    if (!transact(Operation::SelectCandidate, request, response) || response.status != Status::Ok || !response.handled) {
        failEngineSession(edit, cookie);
        return false;
    }
    if (!isValidEdit(edit)) return false;
    if (!applySnapshot(edit, cookie, response.snapshot)) {
        failEngineSession(edit, cookie);
        return false;
    }
    return true;
}

HRESULT XMJDTip::process(EditSession* edit, TfEditCookie cookie) {
    try {
        if (!edit) return E_INVALIDARG;
        // 异步清理只持有旧 composition，允许它在 owner 已切换 epoch 后执行。
        if (edit->action_ == EditAction::CancelComposition) {
            if (!edit->composition_) return S_OK;
            if (composition_ == edit->composition_ && edit->epoch_ == contextEpoch_) releaseInterface(composition_);
            ComPtr<ITfRange> range;
            HRESULT hr = edit->composition_->GetRange(range.GetAddressOf());
            if (SUCCEEDED(hr) && range) hr = range->SetText(cookie, 0, L"", 0);
            else if (SUCCEEDED(hr)) hr = E_FAIL;
            if (SUCCEEDED(hr)) hr = edit->composition_->EndComposition(cookie);
            edit->handled_ = SUCCEEDED(hr);
            return hr;
        }
        if (processingEdit_ || resettingContext_ || !isValidEdit(edit)) return TF_E_DISCONNECTED;
        processingEdit_ = true;
        struct EditGuard { bool& active; ~EditGuard() { active = false; } } guard{processingEdit_};
        if (edit->action_ == EditAction::RecoverComposition) {
            if (!contextUnavailable_) return S_FALSE;
            edit->handled_ = recoverComposition(edit, cookie);
            return edit->handled_ ? S_OK : E_FAIL;
        }
        if (edit->action_ == EditAction::UpdateCandidatePosition) {
            if (!candidateElement_ || edit->revision_ != candidateRevision_ || !uiElementShow_ ||
                !candidateElement_->isShown() || candidateElement_->snapshot().epoch != edit->epoch_) return S_FALSE;
            return positionCandidateWindow(cookie) ? S_OK : S_FALSE;
        }
        if (edit->action_ == EditAction::SelectCandidate) {
            edit->handled_ = processCandidateSelection(edit, cookie);
            return edit->handled_ ? S_OK : S_FALSE;
        }
        if (edit->action_ == EditAction::Cancel) {
            if (!hasEngineSession_) {
                edit->handled_ = endComposition(edit, cookie, true);
                closeCandidateUi();
                return edit->handled_ ? S_OK : E_FAIL;
            }
            Request request;
            Response response;
            if (!transact(Operation::Cancel, request, response) || response.status != Status::Ok) {
                failEngineSession(edit, cookie);
                return E_FAIL;
            }
            if (!isValidEdit(edit)) return TF_E_DISCONNECTED;
            edit->handled_ = applySnapshot(edit, cookie, response.snapshot);
            if (!edit->handled_) failEngineSession(edit, cookie);
            return edit->handled_ ? S_OK : E_FAIL;
        }
        if (edit->action_ == EditAction::FinalizeRaw || edit->action_ == EditAction::FinalizeConverted) {
            if (!hasEngineSession_) return S_FALSE;
            Request request;
            request.convert = edit->action_ == EditAction::FinalizeConverted;
            Response response;
            if (!transact(Operation::Finalize, request, response) || response.status != Status::Ok) {
                failEngineSession(edit, cookie);
                return E_FAIL;
            }
            if (!isValidEdit(edit)) return TF_E_DISCONNECTED;
            edit->handled_ = applySnapshot(edit, cookie, response.snapshot);
            if (!edit->handled_) failEngineSession(edit, cookie);
            return edit->handled_ ? S_OK : E_FAIL;
        }
        if (contextUnavailable_) {
            edit->consumed_ = composition_ != nullptr;
            recoverComposition(edit, cookie);
            scheduleEngineRetry();
            return S_FALSE;
        }
        if (!hasEngineSession_) { scheduleEngineRetry(); return S_FALSE; }

        Request request;
        request.keycode = edit->keycode_;
        request.modifiers = edit->modifiers_;
        Response response;
        // 一旦发送，超时后的处理状态不确定，禁止把同一按键再交给宿主。
        edit->consumed_ = true;
        if (!transact(Operation::ProcessKey, request, response) || response.status != Status::Ok) {
            failEngineSession(edit, cookie);
            edit->handled_ = true;
            return S_OK;
        }
        if (!isValidEdit(edit)) return TF_E_DISCONNECTED;
        if (!response.handled) {
            if (!applySnapshot(edit, cookie, response.snapshot)) {
                failEngineSession(edit, cookie);
                return E_FAIL;
            }
            const bool modifier = edit->keycode_ >= 0xffe1 && edit->keycode_ <= 0xffee;
            if (!modifier && !response.snapshot.rawInput.empty() && hasEngineSession_) {
                Request finalize;
                finalize.convert = false;
                Response finalized;
                if (!transact(Operation::Finalize, finalize, finalized) || finalized.status != Status::Ok ||
                    !isValidEdit(edit) || !applySnapshot(edit, cookie, finalized.snapshot)) {
                    failEngineSession(edit, cookie);
                    return E_FAIL;
                }
            }
            if (!isValidEdit(edit)) return TF_E_DISCONNECTED;
            edit->consumed_ = false;
            edit->handled_ = false;
            return S_FALSE;
        }
        edit->handled_ = applySnapshot(edit, cookie, response.snapshot);
        if (!edit->handled_) failEngineSession(edit, cookie);
        return edit->handled_ ? S_OK : E_FAIL;
    } catch (...) {
        if (edit && edit->consumed_) {
            try { failEngineSession(edit, cookie); } catch (...) { invalidateSession(true); }
        }
        return E_UNEXPECTED;
    }
}

UINT XMJDTip::rimeModifiers() const {
    UINT modifiers = 0;
    if ((GetKeyState(VK_SHIFT) & 0x8000) != 0) modifiers |= kMaskShift;
    if ((GetKeyState(VK_CAPITAL) & 1) != 0) modifiers |= kMaskLock;
    if ((GetKeyState(VK_CONTROL) & 0x8000) != 0) modifiers |= kMaskControl;
    if ((GetKeyState(VK_MENU) & 0x8000) != 0) modifiers |= kMaskAlt;
    if ((GetKeyState(VK_LWIN) & 0x8000) != 0 || (GetKeyState(VK_RWIN) & 0x8000) != 0) modifiers |= kMaskSuper;
    if ((GetKeyState(VK_RMENU) & 0x8000) != 0 && (GetKeyState(VK_LCONTROL) & 0x8000) != 0) modifiers |= kMaskAltGr;
    return modifiers;
}

bool XMJDTip::shouldTestKey(UINT key, bool keyUp) const {
    if (key == VK_CAPITAL) return keyUp ? capsKeyDown_ : !capsKeyDown_;
    const bool modifier = key == VK_SHIFT || key == VK_LSHIFT || key == VK_RSHIFT ||
        key == VK_CONTROL || key == VK_LCONTROL || key == VK_RCONTROL ||
        key == VK_MENU || key == VK_LMENU || key == VK_RMENU || key == VK_LWIN || key == VK_RWIN;
    if (keyUp) return modifier && modifierKeysDown_ != 0;
    if (modifier) return true;
    const bool control = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
    const bool altGr = (GetKeyState(VK_RMENU) & 0x8000) != 0 && (GetKeyState(VK_LCONTROL) & 0x8000) != 0;
    const bool windowsKey = (GetKeyState(VK_LWIN) & 0x8000) != 0 || (GetKeyState(VK_RWIN) & 0x8000) != 0;
    // Preserve ordinary shortcuts; expose the scheme's Control+grave switcher.
    if (windowsKey || ((control || alt) && !altGr && !(control && !alt && key == VK_OEM_3))) return false;
    if (key >= VK_F1 && key <= VK_F24) return true;
    const bool composing = composition_ != nullptr || (candidateElement_ && !candidateElement_->snapshot().values.empty());
    if (key == VK_ESCAPE || key == VK_BACK || key == VK_DELETE || key == VK_RETURN || key == VK_SPACE || key == VK_TAB ||
        key == VK_LEFT || key == VK_RIGHT || key == VK_UP || key == VK_DOWN || key == VK_PRIOR || key == VK_NEXT || key == VK_HOME || key == VK_END) {
        return composing;
    }
    const bool printable = (key >= '0' && key <= '9') || (key >= 'A' && key <= 'Z') ||
        (key >= VK_NUMPAD0 && key <= VK_DIVIDE) || (key >= VK_OEM_1 && key <= VK_OEM_102);
    return printable && !keyUp;
}

bool XMJDTip::translateKey(UINT virtualKey, LPARAM keyData, UINT& keycode, UINT& modifiers) const {
    if (virtualKey == VK_SHIFT) {
        const UINT scan = static_cast<UINT>((keyData >> 16) & 0xff);
        virtualKey = scan == 0x36 ? VK_RSHIFT : VK_LSHIFT;
    }
    if (virtualKey == VK_CONTROL) virtualKey = (keyData & (1LL << 24)) ? VK_RCONTROL : VK_LCONTROL;
    if (virtualKey == VK_MENU) virtualKey = (keyData & (1LL << 24)) ? VK_RMENU : VK_LMENU;
    if (virtualKey >= VK_F1 && virtualKey <= VK_F24) {
        keycode = 0xffbe + virtualKey - VK_F1;
        modifiers = rimeModifiers();
        return true;
    }
    switch (virtualKey) {
    case VK_LSHIFT: keycode = 0xffe1; modifiers = rimeModifiers(); return true;
    case VK_RSHIFT: keycode = 0xffe2; modifiers = rimeModifiers(); return true;
    case VK_LCONTROL: keycode = 0xffe3; modifiers = rimeModifiers(); return true;
    case VK_RCONTROL: keycode = 0xffe4; modifiers = rimeModifiers(); return true;
    case VK_LMENU: keycode = 0xffe9; modifiers = rimeModifiers(); return true;
    case VK_RMENU: keycode = 0xffea; modifiers = rimeModifiers(); return true;
    case VK_LWIN: keycode = 0xffeb; modifiers = rimeModifiers(); return true;
    case VK_RWIN: keycode = 0xffec; modifiers = rimeModifiers(); return true;
    case VK_BACK: keycode = 0xff08; modifiers = rimeModifiers(); return true;
    case VK_TAB: keycode = 0xff09; modifiers = rimeModifiers(); return true;
    case VK_RETURN: keycode = 0xff0d; modifiers = rimeModifiers(); return true;
    case VK_ESCAPE: keycode = 0xff1b; modifiers = rimeModifiers(); return true;
    case VK_SPACE: keycode = 0x20; modifiers = rimeModifiers(); return true;
    case VK_PRIOR: keycode = 0xff55; modifiers = rimeModifiers(); return true;
    case VK_NEXT: keycode = 0xff56; modifiers = rimeModifiers(); return true;
    case VK_END: keycode = 0xff57; modifiers = rimeModifiers(); return true;
    case VK_HOME: keycode = 0xff50; modifiers = rimeModifiers(); return true;
    case VK_LEFT: keycode = 0xff51; modifiers = rimeModifiers(); return true;
    case VK_UP: keycode = 0xff52; modifiers = rimeModifiers(); return true;
    case VK_RIGHT: keycode = 0xff53; modifiers = rimeModifiers(); return true;
    case VK_DOWN: keycode = 0xff54; modifiers = rimeModifiers(); return true;
    case VK_DELETE: keycode = 0xffff; modifiers = rimeModifiers(); return true;
    case VK_CAPITAL: keycode = 0xffe5; modifiers = rimeModifiers() ^ kMaskLock; return true;
    default: break;
    }
    BYTE state[256]{};
    if (!GetKeyboardState(state)) return false;
    modifiers = rimeModifiers();
    const bool altGr = (state[VK_RMENU] & 0x80) != 0 && (state[VK_LCONTROL] & 0x80) != 0;
    state[VK_SHIFT] = state[VK_LSHIFT] = state[VK_RSHIFT] = 0;
    state[VK_CAPITAL] &= 0x80;
    if (!altGr) {
        state[VK_CONTROL] = state[VK_LCONTROL] = state[VK_RCONTROL] = 0;
        state[VK_MENU] = state[VK_LMENU] = state[VK_RMENU] = 0;
    }
    const UINT scan = static_cast<UINT>((keyData >> 16) & 0xff);
    WCHAR translated[4]{};
    const int count = ToUnicodeEx(virtualKey, scan, state, translated, static_cast<int>(std::size(translated)), 0x4, GetKeyboardLayout(0));
    if (count <= 0) return false;
    std::uint32_t codepoint = translated[0];
    if (count >= 2 && translated[0] >= 0xd800 && translated[0] <= 0xdbff && translated[1] >= 0xdc00 && translated[1] <= 0xdfff) {
        codepoint = 0x10000 + ((translated[0] - 0xd800) << 10) + (translated[1] - 0xdc00);
    }
    if (codepoint < 0x20 || (codepoint >= 0x7f && codepoint < 0xa0)) return false;
    keycode = codepoint <= 0xff ? codepoint : 0x01000000u | codepoint;
    return true;
}

HRESULT XMJDTip::OnTestKeyDown(ITfContext* context, WPARAM key, LPARAM, BOOL* eaten) {
    if (!eaten) return E_POINTER;
    *eaten = FALSE;
    if (!activated_ || !context || !isCurrentContext(context)) return S_OK;
    *eaten = shouldTestKey(static_cast<UINT>(key), false) ? TRUE : FALSE;
    return S_OK;
}
HRESULT XMJDTip::OnTestKeyUp(ITfContext* context, WPARAM key, LPARAM, BOOL* eaten) {
    if (!eaten) return E_POINTER;
    *eaten = FALSE;
    if (activated_ && context && isCurrentContext(context) && shouldTestKey(static_cast<UINT>(key), true)) *eaten = TRUE;
    return S_OK;
}
HRESULT XMJDTip::handleKey(ITfContext* context, WPARAM key, LPARAM keyData, bool keyUp, BOOL* eaten) {
    if (!eaten) return E_POINTER;
    *eaten = FALSE;
    try {
        if (!activated_ || !context || !isCurrentContext(context) || !shouldTestKey(static_cast<UINT>(key), keyUp)) return S_OK;
        if (!ensureContext(context)) return S_OK;
        UINT keycode = 0;
        UINT modifiers = 0;
        if (!translateKey(static_cast<UINT>(key), keyData, keycode, modifiers)) return S_OK;
        if (keyUp) modifiers |= kReleaseMask;
        const bool modifier = keycode >= 0xffe1 && keycode <= 0xffee && keycode != 0xffe5;
        const UINT modifierBit = modifier ? 1u << (keycode - 0xffe1) : 0u;
        // Rime must see modifier chords as well as Shift taps; otherwise a
        // Ctrl+Shift shortcut could be mistaken for a lone Shift release.
        if (modifier) {
            if (keyUp) modifierKeysDown_ &= ~modifierBit;
            else modifierKeysDown_ |= modifierBit;
        }
        EditSession* edit = new (std::nothrow) EditSession(this, EditAction::ProcessKey, keycode, modifiers, 0, 0, contextEpoch_);
        if (!edit) return E_OUTOFMEMORY;
        HRESULT sessionResult = E_FAIL;
        const HRESULT hr = requestEdit(context, edit, TF_ES_SYNC | TF_ES_READWRITE, &sessionResult);
        *eaten = edit->consumed() || (SUCCEEDED(hr) && SUCCEEDED(sessionResult) && edit->handled()) ? TRUE : FALSE;
        if (modifier) *eaten = FALSE;
        if (key == VK_CAPITAL) capsKeyDown_ = *eaten != FALSE;
        edit->Release();
        return S_OK;
    } catch (...) {
        *eaten = FALSE;
        return E_UNEXPECTED;
    }
}
HRESULT XMJDTip::OnKeyDown(ITfContext* context, WPARAM key, LPARAM keyData, BOOL* eaten) {
    return handleKey(context, key, keyData, false, eaten);
}
HRESULT XMJDTip::OnKeyUp(ITfContext* context, WPARAM key, LPARAM keyData, BOOL* eaten) {
    if (key == VK_CAPITAL) {
        if (!eaten) return E_POINTER;
        *eaten = capsKeyDown_ && activated_ && context && isCurrentContext(context) ? TRUE : FALSE;
        capsKeyDown_ = false;
        return S_OK;
    }
    return handleKey(context, key, keyData, true, eaten);
}
HRESULT XMJDTip::OnPreservedKey(ITfContext*, REFGUID, BOOL* eaten) {
    if (!eaten) return E_POINTER;
    *eaten = FALSE;
    return S_OK;
}

HRESULT XMJDTip::OnCompositionTerminated(TfEditCookie, ITfComposition* composition) {
    if (composition && composition_ == composition) {
        releaseInterface(composition_);
        compositionTerminated_ = true;
        acknowledgedRaw_.clear();
        invalidateSession(true);
        candidateRevision_ = 0;
        if (++contextEpoch_ == 0) ++contextEpoch_;
        closeCandidateUi();
        scheduleEngineRetry();
    }
    return S_OK;
}

HRESULT XMJDTip::OnActivated(DWORD, LANGID, REFCLSID clsid, REFGUID, REFGUID profile, HKL, DWORD flags) {
    if (!activated_) return S_OK;
    const bool ownProfile = clsid == kTipClsid && profile == kProfileGuid;
    const bool active = (flags & TF_IPSINK_FLAG_ACTIVE) != 0;
    if (ownProfile || !active) return S_OK;
    try {
        if (context_ && hasEngineSession_ && !contextUnavailable_ && isCurrentContext(context_)) {
            EditSession* edit = new (std::nothrow) EditSession(this, EditAction::FinalizeRaw, 0, 0, 0, 0, contextEpoch_);
            if (edit) {
                HRESULT result = E_FAIL;
                requestEdit(context_, edit, TF_ES_SYNC | TF_ES_READWRITE, &result);
                edit->Release();
            }
        }
        resetForFocusChange();
        return S_OK;
    } catch (...) {
        return E_UNEXPECTED;
    }
}

void XMJDTip::resetForFocusChange() {
    if (resettingContext_) return;
    resettingContext_ = true;
    closeCandidateUi();
    cancelEngineRetry();
    if (composition_ && context_) {
        EditSession* edit = new (std::nothrow) EditSession(this, EditAction::CancelComposition, 0, 0, 0, 0, contextEpoch_);
        if (edit) {
            HRESULT result = E_FAIL;
            const HRESULT hr = requestEdit(context_, edit, TF_ES_SYNC | TF_ES_READWRITE, &result);
            if (FAILED(hr) || FAILED(result)) {
                requestEdit(context_, edit, TF_ES_ASYNC | TF_ES_READWRITE, &result);
            }
            edit->Release();
        }
    }
    releaseInterface(composition_);
    releaseInterface(candidateElement_);
    releaseInterface(context_);
    releaseInterface(documentManager_);
    invalidateSession(true);
    if (++contextEpoch_ == 0) ++contextEpoch_;
    sequence_ = 0;
    candidateRevision_ = 0;
    acknowledgedRaw_.clear();
    contextUnavailable_ = false;
    compositionTerminated_ = false;
    capsKeyDown_ = false;
    modifierKeysDown_ = 0;
    resettingContext_ = false;
}

HRESULT XMJDTip::OnInitDocumentMgr(ITfDocumentMgr*) { return S_OK; }
HRESULT XMJDTip::OnUninitDocumentMgr(ITfDocumentMgr* document) {
    if (document && document == documentManager_) resetForFocusChange();
    return S_OK;
}
HRESULT XMJDTip::OnSetFocus(ITfDocumentMgr* focused, ITfDocumentMgr*) {
    try {
        if (!activated_ || resettingContext_) return S_OK;
        // Do not wait until the next key to invalidate candidates in another field.
        if (focused != documentManager_) resetForFocusChange();
        if (focused && foreground_) return OnSetFocus(TRUE);
        return S_OK;
    } catch (...) { return E_UNEXPECTED; }
}
HRESULT XMJDTip::OnPushContext(ITfContext*) {
    return activated_ && foreground_ ? OnSetFocus(TRUE) : S_OK;
}
HRESULT XMJDTip::OnPopContext(ITfContext* context) {
    if (context && context == context_) resetForFocusChange();
    return activated_ && foreground_ ? OnSetFocus(TRUE) : S_OK;
}

HRESULT XMJDTip::OnSetFocus(BOOL foreground) {
    try {
        foreground_ = foreground != FALSE;
        if (!foreground_) {
            resetForFocusChange();
            return S_OK;
        }
        ITfContext* context = nullptr;
        ITfDocumentMgr* document = nullptr;
        if (SUCCEEDED(getFocusedContext(&context, &document))) {
            if (ensureContext(context) && !hasEngineSession_) scheduleEngineRetry();
            releaseInterface(context);
            releaseInterface(document);
        } else {
            resetForFocusChange();
        }
        return S_OK;
    } catch (...) {
        return E_UNEXPECTED;
    }
}

LRESULT CALLBACK XMJDTip::popupWndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    XMJDTip* owner = reinterpret_cast<XMJDTip*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        owner = static_cast<XMJDTip*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(owner));
        return TRUE;
    }
    if (message == WM_NCDESTROY) {
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        if (owner && owner->candidateWindow_ == window) {
            owner->candidateWindow_ = nullptr;
            owner->engineRetryScheduled_ = false;
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }
    ComPtr<XMJDTip> keepAlive = owner;
    if (message == WM_TIMER && wParam == kEngineRetryTimerId && owner) {
        try {
            if (!owner->processingEdit_) owner->retryEngineSession();
        } catch (...) {
            owner->invalidateSession(true);
        }
        return 0;
    }
    if (message == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if (message == WM_ERASEBKGND) return 1;
    if (message == WM_PAINT) {
        PAINTSTRUCT paint{};
        BeginPaint(window, &paint);
        EndPaint(window, &paint);
        return 0;
    }
    if (message == WM_DPICHANGED && owner && owner->candidateRenderer_) {
        const UINT dpi = GetDpiForWindow(window);
        owner->candidateRenderer_->repaint(window, static_cast<int>(dpi == 0 ? 96 : dpi));
        return 0;
    }
    if (message == WM_LBUTTONUP && owner && owner->candidateElement_ && owner->candidateRenderer_) {
        const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        const int index = owner->candidateRenderer_->hitTest(point);
        if (index >= 0) {
            owner->candidateClicked(static_cast<UINT>(index), owner->candidateElement_->snapshot().revision);
        }
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

CandidateElement::CandidateElement(XMJDTip* owner, ITfContext* context) : owner_(owner), context_(context) {
    owner_->AddRef();
    if (context_) context_->AddRef();
    ++g_objectCount;
}
CandidateElement::~CandidateElement() {
    detach();
    --g_objectCount;
}
void CandidateElement::detach() noexcept {
    shown_ = FALSE;
    snapshot_.epoch = 0;
    releaseInterface(context_);
    releaseInterface(owner_);
}
HRESULT STDMETHODCALLTYPE CandidateElement::QueryInterface(REFIID riid, void** object) {
    if (!object) return E_POINTER;
    *object = nullptr;
    if (riid == IID_IUnknown || riid == IID_ITfUIElement || riid == IID_ITfCandidateListUIElement) {
        *object = static_cast<ITfCandidateListUIElement*>(this);
        AddRef();
        return S_OK;
    }
    return E_NOINTERFACE;
}
ULONG STDMETHODCALLTYPE CandidateElement::AddRef() { return ++refs_; }
ULONG STDMETHODCALLTYPE CandidateElement::Release() {
    const ULONG remaining = --refs_;
    if (remaining == 0) delete this;
    return remaining;
}
HRESULT STDMETHODCALLTYPE CandidateElement::GetDescription(BSTR* description) {
    if (!description) return E_POINTER;
    *description = SysAllocString(L"XMJD Candidates");
    return *description ? S_OK : E_OUTOFMEMORY;
}
HRESULT STDMETHODCALLTYPE CandidateElement::GetGUID(GUID* guid) {
    if (!guid) return E_POINTER;
    *guid = kCandidateElementGuid;
    return S_OK;
}
HRESULT STDMETHODCALLTYPE CandidateElement::Show(BOOL show) {
    if (!owner_ || owner_->candidateElement_ != this || snapshot_.epoch != owner_->contextEpoch_) {
        shown_ = FALSE;
        return TF_E_DISCONNECTED;
    }
    shown_ = show != FALSE && owner_->uiElementShow_ && !owner_->uiLess_ ? TRUE : FALSE;
    owner_->candidateVisibilityChanged(shown_ != FALSE);
    return S_OK;
}
HRESULT STDMETHODCALLTYPE CandidateElement::IsShown(BOOL* shown) {
    if (!shown) return E_POINTER;
    *shown = shown_;
    return S_OK;
}
HRESULT STDMETHODCALLTYPE CandidateElement::GetUpdatedFlags(DWORD* flags) {
    if (!flags) return E_POINTER;
    *flags = updatedFlags_;
    updatedFlags_ = 0;
    return S_OK;
}
HRESULT STDMETHODCALLTYPE CandidateElement::GetDocumentMgr(ITfDocumentMgr** documentManager) {
    if (!documentManager) return E_POINTER;
    *documentManager = nullptr;
    return context_ ? context_->GetDocumentMgr(documentManager) : TF_E_DISCONNECTED;
}
HRESULT STDMETHODCALLTYPE CandidateElement::GetCount(UINT* count) {
    if (!count) return E_POINTER;
    *count = static_cast<UINT>(snapshot_.values.size());
    return S_OK;
}
HRESULT STDMETHODCALLTYPE CandidateElement::GetSelection(UINT* index) {
    if (!index) return E_POINTER;
    if (snapshot_.values.empty()) return S_FALSE;
    *index = snapshot_.selected;
    return S_OK;
}
HRESULT STDMETHODCALLTYPE CandidateElement::GetString(UINT index, BSTR* text) {
    if (!text) return E_POINTER;
    *text = nullptr;
    if (index >= snapshot_.values.size()) return E_INVALIDARG;
    *text = SysAllocString(snapshot_.values[index].text.c_str());
    return *text ? S_OK : E_OUTOFMEMORY;
}
HRESULT STDMETHODCALLTYPE CandidateElement::GetPageIndex(UINT* indexes, UINT size, UINT* pageCount) {
    if (!pageCount) return E_POINTER;
    *pageCount = snapshot_.values.empty() ? 0u : 1u;
    if (!indexes) return S_OK;
    if (size < *pageCount) return E_INVALIDARG;
    if (*pageCount != 0) std::fill_n(indexes, *pageCount, 0u);
    return S_OK;
}
HRESULT STDMETHODCALLTYPE CandidateElement::SetPageIndex(UINT* indexes, UINT size) {
    if (snapshot_.values.empty()) return size == 0 ? S_OK : E_INVALIDARG;
    if (!indexes || size != 1 || indexes[0] != 0) return E_INVALIDARG;
    return S_OK;
}
HRESULT STDMETHODCALLTYPE CandidateElement::GetCurrentPage(UINT* page) {
    if (!page) return E_POINTER;
    *page = 0;
    return S_OK;
}
void CandidateElement::update(const Snapshot& snapshot) {
    std::vector<CandidateValue> next;
    next.reserve(snapshot.candidates.size());
    for (const auto& candidate : snapshot.candidates) {
        next.push_back({toWide(candidate.label), toWide(candidate.text), toWide(candidate.comment)});
    }
    if (next.size() != snapshot_.values.size()) updatedFlags_ |= TF_CLUIE_COUNT;
    if (snapshot.highlightedIndex != snapshot_.selected) updatedFlags_ |= TF_CLUIE_SELECTION;
    if (next.size() != snapshot_.values.size() || snapshot.revision != snapshot_.revision) updatedFlags_ |= TF_CLUIE_STRING;
    if (snapshot.revision != snapshot_.revision || snapshot.page != snapshot_.page || snapshot.lastPage != snapshot_.lastPage) {
        updatedFlags_ |= TF_CLUIE_PAGEINDEX | TF_CLUIE_CURRENTPAGE;
    }
    snapshot_.values = std::move(next);
    snapshot_.selected = snapshot_.values.empty() ? 0u : std::min<UINT>(snapshot.highlightedIndex, static_cast<UINT>(snapshot_.values.size() - 1));
    snapshot_.revision = snapshot.revision;
    snapshot_.epoch = owner_->contextEpoch_;
    snapshot_.page = snapshot.page;
    snapshot_.lastPage = snapshot.lastPage;
    if (owner_->candidateWindow_) InvalidateRect(owner_->candidateWindow_, nullptr, TRUE);
}

EditSession::EditSession(XMJDTip* owner, EditAction action, UINT keycode, UINT modifiers,
                         UINT candidateIndex, std::uint64_t revision, std::uint64_t epoch)
    : owner_(owner), context_(owner->context_), action_(action), keycode_(keycode), modifiers_(modifiers), candidateIndex_(candidateIndex),
      revision_(revision), epoch_(epoch) {
    owner_->AddRef();
    if (context_) context_->AddRef();
    if (action_ == EditAction::CancelComposition && owner_->composition_) {
        composition_ = owner_->composition_;
        composition_->AddRef();
    }
    ++g_objectCount;
}
EditSession::~EditSession() {
    releaseInterface(composition_);
    releaseInterface(context_);
    owner_->Release();
    --g_objectCount;
}
HRESULT STDMETHODCALLTYPE EditSession::QueryInterface(REFIID riid, void** object) {
    if (!object) return E_POINTER;
    *object = nullptr;
    if (riid != IID_IUnknown && riid != IID_ITfEditSession) return E_NOINTERFACE;
    *object = static_cast<ITfEditSession*>(this);
    AddRef();
    return S_OK;
}
ULONG STDMETHODCALLTYPE EditSession::AddRef() { return ++refs_; }
ULONG STDMETHODCALLTYPE EditSession::Release() {
    const ULONG remaining = --refs_;
    if (remaining == 0) delete this;
    return remaining;
}
HRESULT STDMETHODCALLTYPE EditSession::DoEditSession(TfEditCookie cookie) {
    try { return owner_->process(this, cookie); }
    catch (...) { return E_UNEXPECTED; }
}

} // namespace xmjd::tip

namespace xmjd::tip {

class ClassFactory final : public IClassFactory {
public:
    ClassFactory() { ++g_objectCount; }
    ~ClassFactory() { --g_objectCount; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (riid != IID_IUnknown && riid != IID_IClassFactory) return E_NOINTERFACE;
        *object = static_cast<IClassFactory*>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = --refs_;
        if (remaining == 0) delete this;
        return remaining;
    }
    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer, REFIID riid, void** object) override {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (outer) return CLASS_E_NOAGGREGATION;
        XMJDTip* tip = new (std::nothrow) XMJDTip();
        if (!tip) return E_OUTOFMEMORY;
        const HRESULT hr = tip->QueryInterface(riid, object);
        tip->Release();
        return hr;
    }
    HRESULT STDMETHODCALLTYPE LockServer(BOOL lock) override {
        if (lock) ++g_serverLocks;
        else if (g_serverLocks > 0) --g_serverLocks;
        return S_OK;
    }
private:
    std::atomic<ULONG> refs_{1};
};

} // namespace xmjd::tip

extern "C" BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        xmjd::tip::g_module = instance;
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID clsid, REFIID iid, void** object) {
    using namespace xmjd::tip;
    if (!object) return E_POINTER;
    *object = nullptr;
    if (clsid != kTipClsid) return CLASS_E_CLASSNOTAVAILABLE;
    ClassFactory* factory = new (std::nothrow) ClassFactory();
    if (!factory) return E_OUTOFMEMORY;
    const HRESULT hr = factory->QueryInterface(iid, object);
    factory->Release();
    return hr;
}

STDAPI DllCanUnloadNow() {
    using namespace xmjd::tip;
    if (g_objectCount.load() != 0 || g_serverLocks.load() != 0) return S_FALSE;
    AcquireSRWLockExclusive(&g_popupClassLock);
    bool released = true;
    if (g_popupClassRegistered) {
        released = UnregisterClassW(kPopupClassName, g_module) != FALSE;
        if (released || GetLastError() == ERROR_CLASS_DOES_NOT_EXIST) {
            g_popupClassRegistered = false;
            released = true;
        }
    }
    ReleaseSRWLockExclusive(&g_popupClassLock);
    return released && g_objectCount.load() == 0 && g_serverLocks.load() == 0 ? S_OK : S_FALSE;
}

STDAPI DllRegisterServer() {
    try { return xmjd::tip::writeTipRegistration(); }
    catch (...) { return E_UNEXPECTED; }
}

STDAPI DllUnregisterServer() {
    try { return xmjd::tip::removeTipRegistration(); }
    catch (...) { return E_UNEXPECTED; }
}
