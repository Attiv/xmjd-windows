#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <olectl.h>
#include <msctf.h>
#include <ctfutb.h>
#include <oleauto.h>
#include <shellapi.h>
#include <windowsx.h>
#include <wrl/client.h>
#include <algorithm>
#include <climits>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <new>
#include <string>
#include <utility>

#include "Tip.h"
#include "LangBarItem.h"
#include <shellapi.h>

#include <cstring>
#include "CandidateRenderer.h"
#include "CandidatePopup.h"

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
// Windows apps filter out desktop-only TIPs before ActivateEx. A language-bar
// button or DLL read permissions do not replace this compatibility declaration.
// Keep registration and unregistration on the same list.
const GUID kTipCategories[] = {
    GUID_TFCAT_TIP_KEYBOARD,
    GUID_TFCAT_TIPCAP_UIELEMENTENABLED,
    GUID_TFCAT_TIPCAP_IMMERSIVESUPPORT,
};
// 候选窗右键菜单：输入法内部没有其它入口能打开设置，只能在候选窗上给出。
constexpr UINT kMenuSettings = 1;
constexpr UINT kMenuUserData = 2;
constexpr UINT kMenuLogs = 3;

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

// Report the process that actually loaded this DLL. The foreground window can
// belong to a different process (especially with system input surfaces).
const std::string& currentProcessName() {
    static const std::string name = [] {
        const auto executable = xmjd::win::modulePath();
        return xmjd::win::narrow(executable.filename().wstring());
    }();
    return name;
}

UINT shiftKeyCode(UINT virtualKey, LPARAM keyData) noexcept {
    if (virtualKey == VK_LSHIFT) return kLeftShiftKey;
    if (virtualKey == VK_RSHIFT) return kRightShiftKey;
    if (virtualKey != VK_SHIFT) return 0;
    return ((keyData >> 16) & 0xff) == 0x36 ? kRightShiftKey : kLeftShiftKey;
}

// Key diagnostics intentionally record categories and outcomes, never input values.
void logKeyOutcome(unsigned keycode, bool handled, std::size_t commit, std::size_t preedit,
                   std::size_t input, bool ascii, std::uint64_t ipcSequence, bool keyUp) {
    const char* keyClass = keycode == kLeftShiftKey ? "left-shift" :
        keycode == kRightShiftKey ? "right-shift" :
        ((keycode >= 'a' && keycode <= 'z') || (keycode >= 'A' && keycode <= 'Z')) ? "letter" :
        ((keycode >= '0' && keycode <= '9') ? "digit" : (keycode >= 0xffe1 ? "modifier" : "other"));
    char debugLine[256]{};
    std::snprintf(debugLine, sizeof(debugLine),
                  "XMJD [%s pid=%lu] ipc=%llu edge=%s key=%s handled=%d commit=%zuB preedit=%zuB input=%zuB ascii=%d\n",
                  currentProcessName().c_str(), GetCurrentProcessId(),
                  static_cast<unsigned long long>(ipcSequence), keyUp ? "up" : "down", keyClass,
                  handled ? 1 : 0, commit, preedit, input, ascii ? 1 : 0);
    OutputDebugStringA(debugLine);

    const auto root = xmjd::win::dataRoot();
    if (root.empty()) return;
    const auto marker = root / L"Logs" / L"keys.debug";
    std::error_code ec;
    if (!std::filesystem::exists(marker, ec)) return;
    std::ofstream stream(root / L"Logs" / L"keys.log", std::ios::app | std::ios::binary);
    if (!stream) return;
    SYSTEMTIME time{};
    GetLocalTime(&time);
    char line[240]{};
    std::snprintf(line, sizeof(line),
                  "%02u:%02u:%02u [%s] ipc=%llu edge=%s key=%s handled=%d commit=%zuB preedit=%zuB input=%zuB ascii=%d\n",
                  static_cast<unsigned>(time.wHour), static_cast<unsigned>(time.wMinute),
                  static_cast<unsigned>(time.wSecond), currentProcessName().c_str(),
                  static_cast<unsigned long long>(ipcSequence), keyUp ? "up" : "down", keyClass,
                  handled ? 1 : 0, commit, preedit, input, ascii ? 1 : 0);
    stream << line;
}

const char* callbackKeyClass(UINT key, LPARAM keyData) noexcept {
    const UINT shift = shiftKeyCode(key, keyData);
    if (shift == kLeftShiftKey) return "left-shift";
    if (shift == kRightShiftKey) return "right-shift";
    if ((key >= 'A' && key <= 'Z') || (key >= 'a' && key <= 'z')) return "letter";
    if (key >= '0' && key <= '9') return "digit";
    if (key == VK_CONTROL || key == VK_LCONTROL || key == VK_RCONTROL ||
        key == VK_MENU || key == VK_LMENU || key == VK_RMENU || key == VK_LWIN || key == VK_RWIN) return "modifier";
    return "other";
}

void logKeyCallback(const char* callback, UINT key, LPARAM keyData, ITfContext* context,
                    BOOL* eaten, HRESULT result) noexcept {
    static std::atomic<std::uint64_t> sequence{0};
    char line[256]{};
    std::snprintf(line, sizeof(line),
                  "XMJD [%s pid=%lu tid=%lu] callback=%llu name=%s key=%s context=%u eaten=%d hr=0x%08lX\n",
                  currentProcessName().c_str(), GetCurrentProcessId(), GetCurrentThreadId(),
                  static_cast<unsigned long long>(sequence.fetch_add(1, std::memory_order_relaxed) + 1),
                  callback, callbackKeyClass(key, keyData), context ? 1u : 0u,
                  eaten ? (*eaten ? 1 : 0) : -1, static_cast<unsigned long>(result));
    OutputDebugStringA(line);
}

// 语言栏按钮点击后打开设置程序。DLL 位于 <安装目录>\Tip\x64\ 下。
void openSettingsFromTip() {
    const std::wstring dll = modulePath();
    if (dll.empty()) return;
    std::filesystem::path directory(dll);
    for (int level = 0; level < 3; ++level) directory = directory.parent_path();
    std::error_code ec;
    const auto executable = directory / L"XMJDSettings.exe";
    if (!std::filesystem::is_regular_file(executable, ec)) return;
    ShellExecuteW(nullptr, L"open", executable.c_str(), nullptr, directory.c_str(), SW_SHOWNORMAL);
}

void logTipEvent(const char* stage) {
    // Debug output still works when the application's sandbox prevents writing
    // our normal product log. Failure to write that log is not proof of no load.
    char debugLine[512]{};
    std::snprintf(debugLine, sizeof(debugLine), "XMJD [%s pid=%lu tid=%lu] %s\n",
                  currentProcessName().c_str(), GetCurrentProcessId(), GetCurrentThreadId(), stage);
    OutputDebugStringA(debugLine);
    const auto root = xmjd::win::dataRoot();
    if (root.empty()) return;
    const auto directory = root / L"Logs";
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    if (ec) return;
    std::ofstream stream(directory / L"tip.log", std::ios::app | std::ios::binary);
    if (!stream) return;
    SYSTEMTIME time{};
    GetLocalTime(&time);
    const std::string host = currentProcessName();
    char stamp[80]{};
    std::snprintf(stamp, sizeof(stamp), "%04u-%02u-%02u %02u:%02u:%02u [%s] ",
                  static_cast<unsigned>(time.wYear), static_cast<unsigned>(time.wMonth),
                  static_cast<unsigned>(time.wDay), static_cast<unsigned>(time.wHour),
                  static_cast<unsigned>(time.wMinute), static_cast<unsigned>(time.wSecond),
                  host.empty() ? "?" : host.c_str());
    stream << stamp << stage << '\n';
}

void logActivationResult(const char* stage, HRESULT result, DWORD flags) {
    char line[160]{};
    std::snprintf(line, sizeof(line), "%s hr=0x%08lX flags=0x%08lX", stage,
                  static_cast<unsigned long>(result), static_cast<unsigned long>(flags));
    logTipEvent(line);
}

void logFocusState(const char* stage, HRESULT result, bool hasDocument, bool hasContext,
                   bool foreground, bool hasSession, std::uint64_t epoch) noexcept {
    char line[320]{};
    std::snprintf(line, sizeof(line),
                  "XMJD [%s pid=%lu tid=%lu] %s hr=0x%08lX document=%u context=%u foreground=%u session=%u epoch=%llu\n",
                  currentProcessName().c_str(), GetCurrentProcessId(), GetCurrentThreadId(), stage,
                  static_cast<unsigned long>(result), hasDocument ? 1u : 0u,
                  hasContext ? 1u : 0u, foreground ? 1u : 0u, hasSession ? 1u : 0u,
                  static_cast<unsigned long long>(epoch));
    OutputDebugStringA(line);
}

void logModeNotification(const char* stage, bool asciiMode, HRESULT result) noexcept {
    char line[256]{};
    std::snprintf(line, sizeof(line), "XMJD [%s pid=%lu tid=%lu] %s mode=%s notify-hr=0x%08lX\n",
                  currentProcessName().c_str(), GetCurrentProcessId(), GetCurrentThreadId(), stage,
                  asciiMode ? "english" : "chinese", static_cast<unsigned long>(result));
    OutputDebugStringA(line);
}

void logCandidateStage(const char* stage, DWORD detail = ERROR_SUCCESS) {
    static std::atomic<ULONGLONG> lastWrite{0};
    const ULONGLONG now = GetTickCount64();
    ULONGLONG previous = lastWrite.load();
    if (now - previous < 1000) return;
    if (!lastWrite.compare_exchange_strong(previous, now)) return;
    char event[160]{};
    std::snprintf(event, sizeof(event), "%s detail=0x%08lX", stage, static_cast<unsigned long>(detail));
    logTipEvent(event);
}

void logTipWin32Failure(const char* stage, unsigned long error) {
    char line[160]{};
    std::snprintf(line, sizeof(line), "%s win32=%lu", stage, error);
    logTipEvent(line);
}

void logTipHresultFailure(const char* stage, HRESULT result) {
    char line[160]{};
    std::snprintf(line, sizeof(line), "%s hr=0x%08lX", stage,
                  static_cast<unsigned long>(result));
    logTipEvent(line);
}

bool readHostExecutable(std::wstring& executable) {
    executable.clear();
    const std::wstring tipModule = modulePath();
    if (!tipModule.empty()) {
        std::filesystem::path install(std::filesystem::path(tipModule).parent_path());
        for (int level = 0; level < 2 && !install.empty(); ++level) install = install.parent_path();
        if (!install.empty()) {
            const auto candidate = install / L"XMJDHost.exe";
            std::error_code ec;
            if (candidate.is_absolute() && std::filesystem::is_regular_file(candidate, ec) && !ec) {
                executable = candidate.wstring();
                return true;
            }
        }
    }
    HKEY key = nullptr;
    const LONG openResult = RegOpenKeyExW(HKEY_LOCAL_MACHINE, kInstallMetadataKey, 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key);
    if (openResult != ERROR_SUCCESS) {
        logTipWin32Failure("host-path registry-open-failed", static_cast<unsigned long>(openResult));
        return false;
    }
    DWORD type = 0;
    DWORD bytes = 0;
    LONG query = RegQueryValueExW(key, kHostExecutableValue, nullptr, &type, nullptr, &bytes);
    if (query != ERROR_SUCCESS || type != REG_SZ || bytes < 2 * sizeof(wchar_t) ||
        bytes > 32768 * sizeof(wchar_t) || bytes % sizeof(wchar_t) != 0) {
        RegCloseKey(key);
        logTipWin32Failure("host-path registry-value-invalid",
                           query == ERROR_SUCCESS ? ERROR_INVALID_DATA : static_cast<unsigned long>(query));
        return false;
    }
    executable.assign(bytes / sizeof(wchar_t), L'\0');
    query = RegQueryValueExW(key, kHostExecutableValue, nullptr, &type,
                             reinterpret_cast<BYTE*>(executable.data()), &bytes);
    RegCloseKey(key);
    if (query != ERROR_SUCCESS || type != REG_SZ || bytes < 2 * sizeof(wchar_t) ||
        bytes % sizeof(wchar_t) != 0) {
        logTipWin32Failure("host-path registry-read-failed",
                           query == ERROR_SUCCESS ? ERROR_INVALID_DATA : static_cast<unsigned long>(query));
        return false;
    }
    executable.resize(bytes / sizeof(wchar_t));
    if (executable.empty() || executable.back() != L'\0') return false;
    executable.pop_back();
    if (executable.empty() || executable.find_first_of(L"\"\r\n") != std::wstring::npos ||
        executable.find(L'\0') != std::wstring::npos) return false;
    return true;
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
    if (!manager) {
        logActivationResult("activate-invalid-manager", E_INVALIDARG, flags);
        return E_INVALIDARG;
    }
    if (activated_) {
        logActivationResult("activate-already-active", TF_E_ALREADY_EXISTS, flags);
        return TF_E_ALREADY_EXISTS;
    }
    logActivationResult("activate-enter", S_OK, flags);
    const auto failActivation = [this, flags](const char* stage, HRESULT result) {
        logActivationResult(stage, result, flags);
        deactivate();
        return result;
    };
    try {
        threadManager_ = manager;
        threadManager_->AddRef();
        clientId_ = clientId;
        activationFlags_ = flags;
        candidateCompositionFailed_ = false;
        allowCandidateAppearance_ = true;
        // 是否由应用自绘候选只能看 BeginUIElement 的 show 返回值；
        // TF_TMF_UIELEMENTENABLEDONLY 只说明线程处于 UILess 模式，
        // 应用仍可能允许输入法自己画候选窗，不能据此关掉候选。
        HRESULT hr = threadManager_->QueryInterface(IID_PPV_ARGS(&keystrokeManager_));
        if (FAILED(hr)) return failActivation("activate-fail query-keystroke", hr);
        hr = threadManager_->QueryInterface(IID_PPV_ARGS(&uiElementManager_));
        if (FAILED(hr)) return failActivation("activate-fail query-ui-element", hr);
        hr = threadManager_->QueryInterface(IID_PPV_ARGS(&threadSource_));
        if (FAILED(hr)) return failActivation("activate-fail query-source", hr);
        hr = keystrokeManager_->AdviseKeyEventSink(clientId_, this, TRUE);
        if (FAILED(hr)) return failActivation("activate-fail key-sink", hr);
        keySinkAdvised_ = true;
        logTipEvent("activate-key-sink-ok");
        logTipEvent("activate-query-ok");
        hr = threadSource_->AdviseSink(IID_ITfThreadMgrEventSink,
                                       static_cast<ITfThreadMgrEventSink*>(this), &threadSinkCookie_);
        if (FAILED(hr)) return failActivation("activate-fail thread-sink", hr);
        hr = threadSource_->AdviseSink(IID_ITfInputProcessorProfileActivationSink,
                                       static_cast<ITfInputProcessorProfileActivationSink*>(this), &profileSinkCookie_);
        if (FAILED(hr)) return failActivation("activate-fail profile-sink", hr);
        activated_ = true;
        foreground_ = true;
        // Register the class on activation, but create the candidate popup only
        // when an actual TSF view provides its owner.
        (void)ensurePopupClass();
        ITfContext* focusedContext = nullptr;
        ITfDocumentMgr* focusedDocument = nullptr;
        if (SUCCEEDED(getFocusedContext(&focusedContext, &focusedDocument))) {
            const bool contextReady = ensureContext(focusedContext);
            releaseInterface(focusedContext);
            releaseInterface(focusedDocument);
            if (contextReady) scheduleEngineRetry();
        }
        createLangBarItem();
        logActivationResult("activate-ok", S_OK, flags);
        return S_OK;
    } catch (...) {
        return failActivation("activate-fail exception", E_UNEXPECTED);
    }
}
HRESULT STDMETHODCALLTYPE XMJDTip::Deactivate() {
    deactivate();
    return S_OK;
}

void XMJDTip::createLangBarItem() {
    if (langBarItem_ || !threadManager_) return;
    ITfLangBarItemMgr* manager = nullptr;
    if (FAILED(threadManager_->QueryInterface(IID_PPV_ARGS(&manager))) || !manager) return;
    auto* item = new (std::nothrow) LangBarItem(GetModuleHandleW(L"XMJDTip.dll"), kTipClsid, openSettingsFromTip);
    if (!item) {
        manager->Release();
        return;
    }
    const HRESULT added = manager->AddItem(item);
    if (FAILED(added)) {
        // 加不上不算致命：输入法本身照常工作，只是没有语言栏按钮。
        item->Release();
        manager->Release();
        logActivationResult("langbar-add-failed", added, activationFlags_);
        return;
    }
    langBarManager_ = manager;
    langBarItem_ = item;
    const HRESULT modeRefresh = item->setAsciiMode(asciiMode_);
    logModeNotification("langbar-initial-mode-notify", asciiMode_, modeRefresh);
    logTipEvent("langbar-added");
}

void XMJDTip::destroyLangBarItem() {
    if (langBarManager_ && langBarItem_) {
        langBarManager_->RemoveItem(static_cast<ITfLangBarItem*>(langBarItem_));
    }
    if (langBarItem_) {
        langBarItem_->Release();
        langBarItem_ = nullptr;
    }
    if (langBarManager_) {
        langBarManager_->Release();
        langBarManager_ = nullptr;
    }
}

void XMJDTip::deactivate() noexcept {
    destroyLangBarItem();
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
    destroyCandidateWindow();
    if (const HWND window = std::exchange(engineRetryWindow_, nullptr)) {
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
    const HRESULT identityResult = context->QueryInterface(IID_PPV_ARGS(&incoming));
    if (FAILED(identityResult)) {
        logFocusState("context-identity-failed", identityResult, documentManager_ != nullptr, false,
                      foreground_, hasEngineSession_, contextEpoch_);
        return false;
    }
    bool same = false;
    if (context_ && SUCCEEDED(context_->QueryInterface(IID_PPV_ARGS(&current)))) same = current == incoming;
    releaseInterface(incoming);
    releaseInterface(current);
    if (same && !compositionTerminated_) return true;

    // reset 可能释放与传入参数相同的上下文，先保留调用方的目标。
    ComPtr<ITfContext> incomingContext = context;
    resetForFocusChange();
    if (!activated_ || !isCurrentContext(incomingContext.Get())) {
        logFocusState("context-adopt-rejected", TF_E_DISCONNECTED, false, true,
                      foreground_, hasEngineSession_, contextEpoch_);
        return false;
    }
    ComPtr<ITfDocumentMgr> incomingDocument;
    const HRESULT documentResult = incomingContext->GetDocumentMgr(incomingDocument.GetAddressOf());
    if (FAILED(documentResult) || !incomingDocument || !activated_ || !isCurrentContext(incomingContext.Get())) {
        logFocusState("context-document-unavailable", FAILED(documentResult) ? documentResult : TF_E_DISCONNECTED,
                      incomingDocument.Get() != nullptr, true, foreground_, hasEngineSession_, contextEpoch_);
        return false;
    }
    context_ = incomingContext.Detach();
    documentManager_ = incomingDocument.Detach();
    logFocusState("context-adopted", S_OK, true, true, foreground_, hasEngineSession_, contextEpoch_);
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
        if (!result) {
            logTipEvent(operation == Operation::CreateSession ? "engine-session response-missing" : "process-key response-missing");
            return false;
        }
        if (result->id.contextEpoch != request.id.contextEpoch ||
            result->id.connectionEpoch != request.id.connectionEpoch || result->id.sequence != request.id.sequence) {
            logTipEvent(operation == Operation::CreateSession ? "engine-session response-stale" : "process-key response-stale");
            return false;
        }
        if (operation != Operation::CreateSession && result->id.sessionId != request.id.sessionId) {
            logTipEvent("process-key response-session-mismatch");
            return false;
        }
        response = *result;
        if (operation == Operation::CreateSession) {
            char diagnostic[256]{};
            std::snprintf(diagnostic, sizeof(diagnostic),
                          "engine-session-response seq=%llu status=%s handled=%u session=%u",
                          static_cast<unsigned long long>(request.id.sequence),
                          xmjd::statusName(response.status), response.handled ? 1u : 0u,
                          response.id.sessionId != 0 ? 1u : 0u);
            logTipEvent(diagnostic);
            if (response.status == Status::Ok && response.handled) {
                sessionId_ = response.id.sessionId;
                hasEngineSession_ = sessionId_ != 0;
            }
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
        std::wstring executable;
        if (!readHostExecutable(executable)) {
            logTipEvent("engine-connect host-path-unavailable");
            return false;
        }
        const std::filesystem::path hostPath(executable);
        std::error_code error;
        if (!hostPath.is_absolute() || !std::filesystem::is_regular_file(hostPath, error)) {
            logTipEvent("engine-connect host-file-unavailable");
            return false;
        }
        if (!pipe_.connect(hostPath)) {
            char stage[128]{};
            std::snprintf(stage, sizeof(stage), "engine-connect %s",
                          pipe_.connectFailureStage() ? pipe_.connectFailureStage() : "unknown");
            logTipWin32Failure(stage, pipe_.connectFailureCode());
            // Never inherit an elevated or restricted application's token.
            HANDLE token = nullptr;
            if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
                logTipWin32Failure("engine-connect token-open-failed", GetLastError());
                return false;
            }
            TOKEN_ELEVATION elevation{};
            DWORD returned = 0;
            const BOOL elevationRead = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &returned);
            DWORD appContainer = 0;
            const BOOL containerRead = GetTokenInformation(token, TokenIsAppContainer, &appContainer,
                                                           sizeof(appContainer), &returned);
            const bool ordinary = elevationRead && containerRead && !elevation.TokenIsElevated &&
                                  !appContainer && !IsTokenRestricted(token);
            const DWORD tokenError = ordinary ? ERROR_SUCCESS :
                (elevationRead && containerRead ? ERROR_ACCESS_DENIED : GetLastError());
            CloseHandle(token);
            if (!ordinary) {
                logTipWin32Failure("engine-connect launch-token-rejected", tokenError);
                return false;
            }
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
                logTipEvent("engine-connect host-launched");
            } else {
                logTipWin32Failure("engine-connect host-launch-failed", GetLastError());
            }
            return false;
        }
    }
    Request request;
    request.wantsAppearance = allowCandidateAppearance_ && (activationFlags_ & TF_TMF_IMMERSIVEMODE) != 0;
    Response response;
    if (!transact(Operation::CreateSession, request, response)) {
        logTipEvent("engine-session transact-failed");
        pipe_.close();
        if (request.wantsAppearance) {
            // Older Hosts reject the extra field. Reconnect with the plain request once.
            allowCandidateAppearance_ = false;
            if (createEngineSession()) return true;
            allowCandidateAppearance_ = true;
        }
        return false;
    }
    if (response.status != Status::Ok || !response.handled) {
        char stage[128]{};
        std::snprintf(stage, sizeof(stage), "engine-session rejected status=%u handled=%u",
                      static_cast<unsigned>(response.status), response.handled ? 1u : 0u);
        logTipEvent(stage);
        pipe_.close();
        return false;
    }
    if (response.appearance) {
        try {
            xmjd::CandidateStyle style;
            std::string error;
            if (xmjd::parseCandidateStyle(response.appearance->json, style, error)) {
                if (!candidateRenderer_) candidateRenderer_ = std::make_unique<CandidateRenderer>();
                candidateRenderer_->setAppearance(std::move(style), response.appearance->dark);
                logTipEvent("candidate-appearance applied");
            }
        } catch (...) {
            // Candidate appearance is optional and must never disable typing.
        }
    }
    reportImeMode(response.snapshot.asciiMode);
    logTipEvent("engine-session created");
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
    if (!engineRetryWindow_ && ensurePopupClass()) {
        engineRetryWindow_ = CreateWindowExW(0, kPopupClassName, L"", 0,
                                              0, 0, 0, 0, HWND_MESSAGE, nullptr, g_module, this);
        // Keep the pre-existing hidden-window retry route in hosts that
        // disallow message-only windows. This HWND never displays candidates.
        if (!engineRetryWindow_) engineRetryWindow_ = createCandidatePopup(kPopupClassName, g_module, nullptr, this);
    }
    if (engineRetryWindow_) {
        engineRetryScheduled_ = SetTimer(engineRetryWindow_, kEngineRetryTimerId, kEngineRetryIntervalMs, nullptr) != 0;
    }
}

void XMJDTip::cancelEngineRetry() noexcept {
    if (engineRetryWindow_) KillTimer(engineRetryWindow_, kEngineRetryTimerId);
    engineRetryScheduled_ = false;
}

bool XMJDTip::startComposition(EditSession* edit, TfEditCookie cookie) {
    if (!isValidEdit(edit)) return false;
    if (composition_) return true;
    ComPtr<ITfContext> target = edit->context_;
    TF_SELECTION selection{};
    ULONG fetched = 0;
    HRESULT hr = target->GetSelection(cookie, TF_DEFAULT_SELECTION, 1, &selection, &fetched);
    if (FAILED(hr) || fetched != 1 || !selection.range) {
        logTipHresultFailure("composition-start selection-failed", FAILED(hr) ? hr : E_FAIL);
        return false;
    }
    ITfContextComposition* contextComposition = nullptr;
    hr = target->QueryInterface(IID_PPV_ARGS(&contextComposition));
    if (FAILED(hr) || !contextComposition || !isValidEdit(edit)) {
        if (FAILED(hr)) logTipHresultFailure("composition-start context-interface-failed", hr);
        releaseInterface(contextComposition);
        selection.range->Release();
        return false;
    }
    ITfComposition* started = nullptr;
    hr = contextComposition->StartComposition(cookie, selection.range, this, &started);
    contextComposition->Release();
    selection.range->Release();
    if (FAILED(hr) || !started) {
        logTipHresultFailure("composition-start tsf-failed", FAILED(hr) ? hr : E_FAIL);
        return false;
    }
    if (!isValidEdit(edit)) {
        started->EndComposition(cookie);
        started->Release();
        return false;
    }
    composition_ = started;
    // 新组合真的建立起来之后才需要重新 BeginUIElement。放在这里而不是 endComposition：
    // 后者在取消、外部终止时也会走到，反复重挂会让 Chromium 判定组合失效，
    // 表现为整个输入法在那边失灵。
    uiElementAttempted_ = false;
    return true;
}

bool XMJDTip::setCompositionText(EditSession* edit, TfEditCookie cookie, std::wstring_view text, UINT caret) {
    if (!isValidEdit(edit)) return false;
    if (!composition_ && !startComposition(edit, cookie)) return false;
    ComPtr<ITfContext> target = edit->context_;
    ComPtr<ITfComposition> active = composition_;
    ITfRange* range = nullptr;
    HRESULT hr = active->GetRange(&range);
    if (FAILED(hr) || !range) {
        logTipHresultFailure("composition-update get-range-failed", FAILED(hr) ? hr : E_FAIL);
        return false;
    }
    const HRESULT textResult = isValidEdit(edit)
        ? range->SetText(cookie, 0, text.empty() ? L"" : text.data(), static_cast<LONG>(text.size())) : TF_E_DISCONNECTED;
    range->Release();
    if (FAILED(textResult)) {
        logTipHresultFailure("composition-update set-text-failed", textResult);
        return false;
    }
    char diagnostic[96]{};
    std::snprintf(diagnostic, sizeof(diagnostic), "XMJD [%s pid=%lu] composition-update-ok units=%zu\n",
                  currentProcessName().c_str(), GetCurrentProcessId(), text.size());
    OutputDebugStringA(diagnostic);
    if (!isValidEdit(edit) || active.Get() != composition_) return false;
    hr = active->GetRange(&range);
    if (FAILED(hr) || !range) {
        logTipHresultFailure("composition-update refresh-range-failed", FAILED(hr) ? hr : E_FAIL);
        return false;
    }
    ITfRange* caretRange = nullptr;
    hr = range->Clone(&caretRange);
    range->Release();
    if (FAILED(hr) || !caretRange) {
        logTipHresultFailure("composition-update clone-range-failed", FAILED(hr) ? hr : E_FAIL);
        return false;
    }
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
    if (FAILED(hr)) logTipHresultFailure("composition-update caret-failed", hr);
    return SUCCEEDED(hr);
}

bool XMJDTip::commitText(EditSession* edit, TfEditCookie cookie, std::wstring_view text) {
    if (!isValidEdit(edit)) {
        logTipEvent("insert-fail edit-invalid");
        return false;
    }
    if (text.empty()) return true;
    ComPtr<ITfContext> target = edit->context_;
    const wchar_t* value = text.data();
    if (composition_) {
        // 先把组合串就地清掉再按普通文本插入。Chromium 系应用（Electron/Chrome）
        // 对"改写组合串内容后立刻结束组合"处理不稳，会把要提交的字丢掉——
        // 顶功时"提交 + 开新组合"正好是那个形状，所以只在那类应用里吞字。
        if (!endComposition(edit, cookie, true)) {
            logTipEvent("insert-fail composition-end");
            return false;
        }
        if (!isValidEdit(edit)) {
            logTipEvent("insert-fail edit-invalid-after-composition");
            return false;
        }
    }
    ComPtr<ITfInsertAtSelection> insertion;
    HRESULT hr = target->QueryInterface(IID_PPV_ARGS(insertion.GetAddressOf()));
    if (FAILED(hr)) {
        logTipHresultFailure("insert-fail query-interface", hr);
        return false;
    }
    if (!isValidEdit(edit)) return false;
    ComPtr<ITfRange> inserted;
    hr = insertion->InsertTextAtSelection(cookie, 0, value, static_cast<LONG>(text.size()), inserted.GetAddressOf());
    if (FAILED(hr) || !inserted) {
        logTipHresultFailure("insert-fail tsf-insert", FAILED(hr) ? hr : E_FAIL);
        return false;
    }
    char diagnostic[96]{};
    std::snprintf(diagnostic, sizeof(diagnostic), "XMJD [%s pid=%lu] insert-ok units=%zu\n",
                  currentProcessName().c_str(), GetCurrentProcessId(), text.size());
    OutputDebugStringA(diagnostic);
    // 插入已经成功，光标调整失败不得触发重放。
    if (isValidEdit(edit)) {
        hr = inserted->Collapse(cookie, TF_ANCHOR_END);
        if (SUCCEEDED(hr) && isValidEdit(edit)) {
            TF_SELECTION selection{};
            selection.range = inserted.Get();
            selection.style.ase = TF_AE_NONE;
            hr = target->SetSelection(cookie, 1, &selection);
        }
        if (FAILED(hr)) logTipHresultFailure("insert-caret-adjust-failed", hr);
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

// Do not set KEYBOARD_OPENCLOSE: disabling the TIP would also disable Shift.
void XMJDTip::reportImeMode(bool asciiMode) {
    asciiMode_ = asciiMode;
    if (!langBarItem_) {
        logModeNotification("langbar-mode-no-item", asciiMode, S_FALSE);
        return;
    }
    const HRESULT hr = langBarItem_->setAsciiMode(asciiMode);
    logModeNotification("langbar-mode-notify", asciiMode, hr);
}

bool XMJDTip::applySnapshot(EditSession* edit, TfEditCookie cookie, const Snapshot& snapshot) {
    if (!isValidEdit(edit)) return false;
    reportImeMode(snapshot.asciiMode);
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
    if (!candidateElement_) return;
    ComPtr<CandidateElement> element = candidateElement_;
    const std::uint64_t epoch = contextEpoch_;
    element->update(snapshot);
    if (!uiElementAttempted_) {
        uiElementAttempted_ = true;
        // 候选由谁绘制由 BeginUIElement 的 show 参数决定：应用要求自己画
        // （UILess 的终端/编辑器）时返回 FALSE，此时不能抢着画；调用失败或
        // 应用不支持 UIElement 时仍用自带候选窗，否则只能盲打。
        uiElementShow_ = true;
        if (uiElementManager_) {
            ComPtr<ITfUIElementMgr> manager = uiElementManager_;
            BOOL show = TRUE;
            DWORD id = TF_INVALID_UIELEMENTID;
            const HRESULT hr = manager->BeginUIElement(element.Get(), &show, &id);
            if (element.Get() != candidateElement_ || epoch != contextEpoch_) {
                if (SUCCEEDED(hr) && id != TF_INVALID_UIELEMENTID) manager->EndUIElement(id);
                return;
            }
            if (SUCCEEDED(hr)) {
                uiElementId_ = id;
                uiElementShow_ = show != FALSE;
            } else {
                logCandidateStage("begin-ui-element-failed");
            }
        }
        element->Show(uiElementShow_ ? TRUE : FALSE);
    }
    if (element.Get() != candidateElement_ || epoch != contextEpoch_) return;
    if (uiElementId_ != TF_INVALID_UIELEMENTID && uiElementManager_) uiElementManager_->UpdateUIElement(uiElementId_);
    if (element.Get() != candidateElement_ || epoch != contextEpoch_) return;
    if (uiElementShow_ && element->isShown()) {
        if (!positionCandidateWindow(cookie)) showCandidateWindow(false);
    } else {
        if (!uiElementShow_) logCandidateStage("app-draws-ui");
        showCandidateWindow(false);
    }
}

void XMJDTip::closeCandidateUi() noexcept {
    uiElementShow_ = false;
    uiElementAttempted_ = false;
    showCandidateWindow(false);
    CandidateElement* element = std::exchange(candidateElement_, nullptr);
    const DWORD id = std::exchange(uiElementId_, TF_INVALID_UIELEMENTID);
    if (element) element->detach();
    if (id != TF_INVALID_UIELEMENTID && uiElementManager_) uiElementManager_->EndUIElement(id);
    if (element) element->Release();
}

bool XMJDTip::positionCandidateWindow(TfEditCookie cookie) {
    if (!context_ || !composition_ || !candidateElement_ || !uiElementShow_) {
        logCandidateStage("skip:no-window-or-state");
        return false;
    }
    const std::uint64_t epoch = contextEpoch_;
    ComPtr<ITfContext> context = context_;
    ComPtr<ITfComposition> composition = composition_;
    ComPtr<CandidateElement> element = candidateElement_;
    ITfRange* range = nullptr;
    ITfContextView* view = nullptr;
    if (FAILED(composition->GetRange(&range)) || !range || FAILED(context->GetActiveView(&view)) || !view) {
        releaseInterface(range);
        releaseInterface(view);
        logCandidateStage("skip:no-range-or-view");
        return false;
    }
    RECT caret{};
    BOOL clipped = FALSE;
    HRESULT hr = view->GetTextExt(cookie, range, &caret, &clipped);
    if (FAILED(hr) || !usableCandidateCaret(caret)) {
        // If the view cannot locate the full composition, try its insertion caret.
        hr = range->Collapse(cookie, TF_ANCHOR_END);
        if (SUCCEEDED(hr)) hr = view->GetTextExt(cookie, range, &caret, &clipped);
    }
    HWND viewWindow = nullptr;
    if (FAILED(view->GetWnd(&viewWindow))) viewWindow = nullptr;
    if (SUCCEEDED(hr) && !usableCandidateCaret(caret) && (activationFlags_ & TF_TMF_IMMERSIVEMODE)) {
        RECT bounds{};
        if (SUCCEEDED(view->GetScreenExt(&bounds)) && !IsRectEmpty(&bounds)) {
            caret = {bounds.left, bounds.top, bounds.left, std::min(bounds.bottom, bounds.top + 32L)};
        }
    }
    range->Release();
    view->Release();
    if (FAILED(hr) || !usableCandidateCaret(caret)) {
        logCandidateStage("skip:text-ext", static_cast<DWORD>(hr));
        return false;
    }
    if (epoch != contextEpoch_ || context.Get() != context_ || element.Get() != candidateElement_ ||
        !uiElementShow_ || !element->isShown()) {
        logCandidateStage("skip:stale");
        return false;
    }
    if (!isCurrentContext(context.Get())) {
        logCandidateStage("skip:focus");
        return false;
    }
    const bool layered = (activationFlags_ & TF_TMF_IMMERSIVEMODE) == 0;
    const bool composited = !layered && !candidateCompositionFailed_;
    const auto fallback = [&](DWORD error) {
        logCandidateStage("composition-fallback", error);
        candidateCompositionFailed_ = true;
        destroyCandidateWindow();
        return positionCandidateWindow(cookie); // At most one retry, with the opaque presenter.
    };
    const auto presentation = layered ? CandidatePresentation::Layered :
                              composited ? CandidatePresentation::Composition : CandidatePresentation::Opaque;
    if (!ensureCandidateWindow(viewWindow, presentation)) return composited ? fallback(GetLastError()) : false;
    if (epoch != contextEpoch_ || context.Get() != context_ || element.Get() != candidateElement_ ||
        !uiElementShow_ || !element->isShown() || !candidateWindow_) return false;
    HMONITOR monitor = MonitorFromRect(&caret, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{sizeof(info)};
    if (!monitor || !GetMonitorInfoW(monitor, &info)) {
        logCandidateStage("skip:monitor");
        return false;
    }
    const HWND owner = GetWindow(candidateWindow_, GW_OWNER);
    const int dpi = static_cast<int>(GetDpiForWindow(owner ? owner : candidateWindow_));
    if (!candidateRenderer_) candidateRenderer_ = std::make_unique<CandidateRenderer>();
    if (!candidateRenderer_) {
        logCandidateStage("skip:renderer");
        return false;
    }
    const CandidateSnapshot& snapshot = element->snapshot();
    if (!candidateRenderer_->prepare(snapshot.values, snapshot.selected, kMaxPopupRows, dpi == 0 ? 96 : dpi, layered || composited)) {
        logCandidateStage("skip:layout");
        return false;
    }
    const xmjd::CandidateLayout& layout = candidateRenderer_->layout();
    const int width = layout.width;
    const int height = layout.height;
    // 窗口比面板大一圈投影留白：面板左上角要落在光标下方，所以窗口原点要外扩。
    const int margin = candidateRenderer_->shadowMargin();
    LONG x = caret.left;
    LONG y = caret.bottom;
    if (y + height > info.rcWork.bottom) y = caret.top - height;
    x = std::max(info.rcWork.left, std::min(x, info.rcWork.right - width));
    y = std::max(info.rcWork.top, std::min(y, info.rcWork.bottom - height));
    RECT previousBounds{};
    const bool moved = GetWindowRect(candidateWindow_, &previousBounds) &&
        (previousBounds.left != x - margin || previousBounds.top != y - margin ||
         previousBounds.right - previousBounds.left != width + margin * 2 ||
         previousBounds.bottom - previousBounds.top != height + margin * 2);
    if (!SetWindowPos(candidateWindow_, nullptr, x - margin, y - margin,
                      width + margin * 2, height + margin * 2, SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER)) {
        logCandidateStage("skip:set-window-pos", GetLastError());
        return false;
    }
    if (layered && !candidateRenderer_->paint(candidateWindow_, dpi == 0 ? 96 : dpi, x, y)) {
        logCandidateStage("skip:paint", GetLastError());
        return false;
    }
    if (composited) {
        HRESULT result = E_FAIL;
        try {
            result = candidateRenderer_->paintComposited(candidateWindow_);
        } catch (...) {
            // The fallback keeps candidates usable even if pixel allocation failed.
        }
        if (FAILED(result)) return fallback(static_cast<DWORD>(result));
    }
    // Presenting content does not show a hidden window.
    if (!showCandidateWindow(true)) {
        logCandidateStage("skip:show");
        return false;
    }
    if (!layered && !composited) {
        opaqueCandidatePainted_ = false;
        if (!InvalidateRect(candidateWindow_, nullptr, FALSE) || !UpdateWindow(candidateWindow_) ||
            !opaqueCandidatePainted_) {
            logCandidateStage("skip:paint-opaque", GetLastError());
            return false;
        }
    }
    if (moved) NotifyWinEvent(EVENT_OBJECT_IME_CHANGE, candidateWindow_, OBJID_CLIENT, CHILDID_SELF);
    logCandidateStage(composited ? "shown:owned:composition" : !layered ? "shown:owned:opaque" :
                      owner ? "shown:owned" : "shown:desktop-fallback");
    return true;
}

bool XMJDTip::ensurePopupClass() {
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
    return registered;
}

bool XMJDTip::ensureCandidateWindow(HWND viewWindow, CandidatePresentation presentation) {
    if (!ensurePopupClass()) {
        logCandidateStage("skip:register-popup-class", GetLastError());
        return false;
    }
    const auto ensureOwner = [this, presentation](HWND owner) {
        if (candidateWindow_ && GetWindow(candidateWindow_, GW_OWNER) == owner) return true;
        const HWND window = createCandidatePopup(kPopupClassName, g_module, owner, this, presentation);
        if (!window) return false;
        destroyCandidateWindow();
        candidateWindow_ = window;
        return true;
    };
    DWORD error = ERROR_INVALID_WINDOW_HANDLE;
    for (const HWND owner : candidateOwnerChoices(viewWindow, GetFocus(), GetForegroundWindow())) {
        if (!owner || owner == candidateWindow_ || !IsWindowVisible(owner) || IsIconic(owner)) continue;
        if (ensureOwner(owner)) return true;
        error = GetLastError();
    }
    // A detached popup can dismiss the Windows Search/Start surface on click.
    // Keep the legacy unowned fallback only for desktop text controls.
    if (!(activationFlags_ & TF_TMF_IMMERSIVEMODE)) {
        if (ensureOwner(nullptr)) return true;
        error = GetLastError();
    }
    logCandidateStage("skip:create-candidate-window", error);
    SetLastError(error);
    return false;
}

void XMJDTip::destroyCandidateWindow() noexcept {
    showCandidateWindow(false);
    if (const HWND window = std::exchange(candidateWindow_, nullptr)) {
        if (candidateRenderer_) candidateRenderer_->resetComposition();
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        DestroyWindow(window);
    }
}

bool XMJDTip::showCandidateWindow(bool show) {
    if (!candidateWindow_) return !show;
    show = show && uiElementShow_;
    const bool wasVisible = IsWindowVisible(candidateWindow_) != FALSE;
    ShowWindow(candidateWindow_, show ? SW_SHOWNOACTIVATE : SW_HIDE);
    const bool visible = IsWindowVisible(candidateWindow_) != FALSE;
    if (wasVisible != visible) {
        NotifyWinEvent(visible ? EVENT_OBJECT_IME_SHOW : EVENT_OBJECT_IME_HIDE,
                       candidateWindow_, OBJID_CLIENT, CHILDID_SELF);
    }
    return visible == show;
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
        if (!transact(Operation::ProcessKey, request, response)) {
            logTipEvent("process-key transact-failed");
            failEngineSession(edit, cookie);
            edit->handled_ = true;
            return S_OK;
        }
        if (response.status != Status::Ok) {
            char stage[128]{};
            std::snprintf(stage, sizeof(stage), "process-key rejected status=%u",
                          static_cast<unsigned>(response.status));
            logTipEvent(stage);
            failEngineSession(edit, cookie);
            edit->handled_ = true;
            return S_OK;
        }
        if (!isValidEdit(edit)) return TF_E_DISCONNECTED;
        logKeyOutcome(edit->keycode_, response.handled != 0, response.snapshot.commitText.size(),
                      response.snapshot.preedit.size(), response.snapshot.rawInput.size(), response.snapshot.asciiMode,
                      request.id.sequence, (edit->modifiers_ & (kReleaseMask | kShiftTapMask)) != 0);
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
    if (key == VK_CAPITAL) {
        if (asciiMode_) return false;   // 英文模式下 CapsLock 归应用
        return keyUp ? capsKeyDown_ : !capsKeyDown_;
    }
    if (shiftKeyCode(key, 0)) return !keyUp || shiftTap_.pending();
    const bool modifier = key == VK_CONTROL || key == VK_LCONTROL || key == VK_RCONTROL ||
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
    // 英文模式下一个按键都不接管：字母、标点、数字全部原样交给应用。
    // 让引擎把英文字母攒进组合串再提交，只在 TSF 原生应用里看得见；IMM32 类
    // 应用（Notepad4、Notepad++、部分游戏）显示不了组合串，表现就是"打不出英文"。
    if (asciiMode_) return false;
    const bool composing = composition_ != nullptr || (candidateElement_ && !candidateElement_->snapshot().values.empty());
    if (key == VK_ESCAPE || key == VK_BACK || key == VK_DELETE || key == VK_RETURN || key == VK_SPACE || key == VK_TAB ||
        key == VK_LEFT || key == VK_RIGHT || key == VK_UP || key == VK_DOWN || key == VK_PRIOR || key == VK_NEXT || key == VK_HOME || key == VK_END) {
        return composing;
    }
    // 数字只有在候选列表开着时才是选词键。以前这里无条件认领，导致游戏、模拟器这类
    // 自己处理键盘的应用里数字被吃掉、又没东西上屏。
    if ((key >= '0' && key <= '9') || (key >= VK_NUMPAD0 && key <= VK_NUMPAD9)) return composing;
    const bool printable = (key >= 'A' && key <= 'Z') || (key >= VK_OEM_1 && key <= VK_OEM_102) ||
                           (key >= VK_MULTIPLY && key <= VK_DIVIDE);
    return printable && !keyUp;
}

bool XMJDTip::translateKey(UINT virtualKey, LPARAM keyData, UINT& keycode, UINT& modifiers) const {
    if (const UINT shift = shiftKeyCode(virtualKey, keyData)) {
        keycode = shift;
        modifiers = rimeModifiers();
        return true;
    }
    if (virtualKey == VK_CONTROL) virtualKey = (keyData & (1LL << 24)) ? VK_RCONTROL : VK_LCONTROL;
    if (virtualKey == VK_MENU) virtualKey = (keyData & (1LL << 24)) ? VK_RMENU : VK_LMENU;
    if (virtualKey >= VK_F1 && virtualKey <= VK_F24) {
        keycode = 0xffbe + virtualKey - VK_F1;
        modifiers = rimeModifiers();
        return true;
    }
    switch (virtualKey) {
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
    // Shift 必须留着：标点是靠"实际打出来的那个字符"查表的（Shift+/ 是 '?'，
    // 不是 '/'），清掉 Shift 会让引擎把 ？ 打成 、。字母在下面再折回小写，
    // 保证方案里的字母键位和以前一致。
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
    if (codepoint >= 'A' && codepoint <= 'Z') codepoint += 'a' - 'A';
    // 标点表是按"字符"配的（'?' : ？ 、 '/' : ､），实测带 Shift 位反而匹配不上，
    // 字符本身已经带上了 Shift 的结果，这里把 Shift 位清掉。
    modifiers &= ~kMaskShift;
    keycode = codepoint <= 0xff ? codepoint : 0x01000000u | codepoint;
    return true;
}

HRESULT XMJDTip::OnTestKeyDown(ITfContext* context, WPARAM key, LPARAM keyData, BOOL* eaten) {
    if (!eaten) {
        logKeyCallback("OnTestKeyDown", static_cast<UINT>(key), keyData, context, nullptr, E_POINTER);
        return E_POINTER;
    }
    *eaten = FALSE;
    HRESULT result = S_OK;
    if (activated_ && context && isCurrentContext(context)) {
        shiftTap_.observeKeyDown(shiftKeyCode(static_cast<UINT>(key), keyData));
        *eaten = shouldTestKey(static_cast<UINT>(key), false) ? TRUE : FALSE;
    }
    logKeyCallback("OnTestKeyDown", static_cast<UINT>(key), keyData, context, eaten, result);
    return result;
}
HRESULT XMJDTip::OnTestKeyUp(ITfContext* context, WPARAM key, LPARAM keyData, BOOL* eaten) {
    if (!eaten) {
        logKeyCallback("OnTestKeyUp", static_cast<UINT>(key), keyData, context, nullptr, E_POINTER);
        return E_POINTER;
    }
    *eaten = FALSE;
    if (activated_ && context && isCurrentContext(context) && shouldTestKey(static_cast<UINT>(key), true)) *eaten = TRUE;
    logKeyCallback("OnTestKeyUp", static_cast<UINT>(key), keyData, context, eaten, S_OK);
    return S_OK;
}
HRESULT XMJDTip::handleKey(ITfContext* context, WPARAM key, LPARAM keyData, bool keyUp, BOOL* eaten) {
    const char* callback = keyUp ? "OnKeyUp" : "OnKeyDown";
    const auto finish = [&](HRESULT result) {
        logKeyCallback(callback, static_cast<UINT>(key), keyData, context, eaten, result);
        return result;
    };
    if (!eaten) return finish(E_POINTER);
    *eaten = FALSE;
    try {
        if (!activated_ || !context || !isCurrentContext(context)) return finish(S_OK);
        if (!keyUp) shiftTap_.observeKeyDown(shiftKeyCode(static_cast<UINT>(key), keyData));
        if (!shouldTestKey(static_cast<UINT>(key), keyUp)) return finish(S_OK);
        if (!ensureContext(context)) return finish(S_OK);
        UINT keycode = 0;
        UINT modifiers = 0;
        if (!translateKey(static_cast<UINT>(key), keyData, keycode, modifiers)) return finish(S_OK);
        const bool shift = keycode == kLeftShiftKey || keycode == kRightShiftKey;
        if (shift) {
            const int otherShift = keycode == kLeftShiftKey ? VK_RSHIFT : VK_LSHIFT;
            const bool otherModifier = (modifiers & (kMaskControl | kMaskAlt | kMaskSuper)) != 0;
            const bool chord = otherModifier || (GetKeyState(otherShift) & 0x8000) != 0;
            const auto now = GetTickCount64();
            if (keyUp) {
                if (!shiftTap_.release(keycode, now, chord)) return finish(S_OK);
                modifiers = (modifiers & kMaskLock) | kShiftTapMask;
            } else {
                const bool repeated = (keyData & (1LL << 30)) != 0;
                shiftTap_.press(keycode, now, repeated, chord);
                if (shiftTap_.pending() && !hasEngineSession_) retryEngineSession();
                // Let an actual modifier chord cancel any other Rime modifier
                // gesture. An isolated Shift is processed only at release.
                if (!otherModifier || repeated) return finish(S_OK);
                modifiers |= kMaskShift;
            }
        } else if (keyUp) {
            modifiers |= kReleaseMask;
        }
        const bool modifier = keycode >= 0xffe1 && keycode <= 0xffee && keycode != 0xffe5;
        const UINT modifierBit = modifier ? 1u << (keycode - 0xffe1) : 0u;
        if (modifier && !shift) {
            if (keyUp) modifierKeysDown_ &= ~modifierBit;
            else modifierKeysDown_ |= modifierBit;
        }
        EditSession* edit = new (std::nothrow) EditSession(this, EditAction::ProcessKey, keycode, modifiers, 0, 0, contextEpoch_);
        if (!edit) return finish(E_OUTOFMEMORY);
        HRESULT sessionResult = E_FAIL;
        const HRESULT hr = requestEdit(context, edit, TF_ES_SYNC | TF_ES_READWRITE, &sessionResult);
        *eaten = edit->consumed() || (SUCCEEDED(hr) && SUCCEEDED(sessionResult) && edit->handled()) ? TRUE : FALSE;
        if (modifier) *eaten = FALSE;
        if (key == VK_CAPITAL) capsKeyDown_ = *eaten != FALSE;
        edit->Release();
        return finish(S_OK);
    } catch (...) {
        *eaten = FALSE;
        return finish(E_UNEXPECTED);
    }
}
HRESULT XMJDTip::OnKeyDown(ITfContext* context, WPARAM key, LPARAM keyData, BOOL* eaten) {
    return handleKey(context, key, keyData, false, eaten);
}
HRESULT XMJDTip::OnKeyUp(ITfContext* context, WPARAM key, LPARAM keyData, BOOL* eaten) {
    if (key == VK_CAPITAL) {
        if (!eaten) {
            logKeyCallback("OnKeyUp", static_cast<UINT>(key), keyData, context, nullptr, E_POINTER);
            return E_POINTER;
        }
        *eaten = capsKeyDown_ && activated_ && context && isCurrentContext(context) ? TRUE : FALSE;
        capsKeyDown_ = false;
        logKeyCallback("OnKeyUp", static_cast<UINT>(key), keyData, context, eaten, S_OK);
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
    shiftTap_.reset();
    modifierKeysDown_ = 0;
    resettingContext_ = false;
}

HRESULT XMJDTip::OnInitDocumentMgr(ITfDocumentMgr*) {
    logFocusState("document-init", S_OK, true, false, foreground_, hasEngineSession_, contextEpoch_);
    return S_OK;
}
HRESULT XMJDTip::OnUninitDocumentMgr(ITfDocumentMgr* document) {
    const bool current = document && document == documentManager_;
    logFocusState("document-uninit", S_OK, document != nullptr, context_ != nullptr,
                  foreground_, hasEngineSession_, contextEpoch_);
    if (current) resetForFocusChange();
    return S_OK;
}
HRESULT XMJDTip::OnSetFocus(ITfDocumentMgr* focused, ITfDocumentMgr*) {
    try {
        logFocusState("document-focus-callback", S_OK, focused != nullptr, context_ != nullptr,
                      foreground_, hasEngineSession_, contextEpoch_);
        if (!activated_ || resettingContext_) return S_OK;
        // Do not wait until the next key to invalidate candidates in another field.
        if (focused != documentManager_) resetForFocusChange();
        if (focused && foreground_) return OnSetFocus(TRUE);
        return S_OK;
    } catch (...) {
        logFocusState("document-focus-callback-exception", E_UNEXPECTED, focused != nullptr,
                      context_ != nullptr, foreground_, hasEngineSession_, contextEpoch_);
        return E_UNEXPECTED;
    }
}
HRESULT XMJDTip::OnPushContext(ITfContext* context) {
    logFocusState("context-push", S_OK, documentManager_ != nullptr, context != nullptr,
                  foreground_, hasEngineSession_, contextEpoch_);
    return activated_ && foreground_ ? OnSetFocus(TRUE) : S_OK;
}
HRESULT XMJDTip::OnPopContext(ITfContext* context) {
    logFocusState("context-pop", S_OK, documentManager_ != nullptr, context != nullptr,
                  foreground_, hasEngineSession_, contextEpoch_);
    if (context && context == context_) resetForFocusChange();
    return activated_ && foreground_ ? OnSetFocus(TRUE) : S_OK;
}

HRESULT XMJDTip::OnSetFocus(BOOL foreground) {
    try {
        foreground_ = foreground != FALSE;
        logFocusState("foreground-focus", S_OK, documentManager_ != nullptr, context_ != nullptr,
                      foreground_, hasEngineSession_, contextEpoch_);
        if (!foreground_) {
            resetForFocusChange();
            logFocusState("foreground-focus-cleared", S_OK, false, false,
                          foreground_, hasEngineSession_, contextEpoch_);
            return S_OK;
        }
        ITfContext* context = nullptr;
        ITfDocumentMgr* document = nullptr;
        const HRESULT focusResult = getFocusedContext(&context, &document);
        if (SUCCEEDED(focusResult)) {
            const bool adopted = ensureContext(context);
            logFocusState("focused-context-result", adopted ? S_OK : TF_E_DISCONNECTED,
                          document != nullptr, context != nullptr, foreground_,
                          hasEngineSession_, contextEpoch_);
            if (adopted && !hasEngineSession_) scheduleEngineRetry();
            releaseInterface(context);
            releaseInterface(document);
        } else {
            logFocusState("focused-context-failed", focusResult, document != nullptr,
                          context != nullptr, foreground_, hasEngineSession_, contextEpoch_);
            resetForFocusChange();
        }
        return S_OK;
    } catch (...) {
        logFocusState("foreground-focus-exception", E_UNEXPECTED, documentManager_ != nullptr,
                      context_ != nullptr, foreground_, hasEngineSession_, contextEpoch_);
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
            if (owner->candidateRenderer_) owner->candidateRenderer_->resetComposition();
        }
        if (owner && owner->engineRetryWindow_ == window) {
            owner->engineRetryWindow_ = nullptr;
            owner->engineRetryScheduled_ = false;
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }
    ComPtr<XMJDTip> keepAlive = owner;
    if (message == WM_TIMER && wParam == kEngineRetryTimerId && owner && owner->engineRetryWindow_ == window) {
        try {
            if (!owner->processingEdit_) owner->retryEngineSession();
        } catch (...) {
            owner->invalidateSession(true);
        }
        return 0;
    }
    if (message == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if (message == WM_ERASEBKGND) return 1;
    if (message == WM_RBUTTONUP && owner) {
        owner->showContextMenu(window);
        return 0;
    }
    if (message == WM_PAINT) {
        PAINTSTRUCT paint{};
        const HDC dc = BeginPaint(window, &paint);
        if (dc && owner && window == owner->candidateWindow_ && owner->candidateRenderer_ &&
            (GetWindowLongPtrW(window, GWL_EXSTYLE) & (WS_EX_LAYERED | WS_EX_NOREDIRECTIONBITMAP)) == 0) {
            try {
                RECT client{};
                owner->opaqueCandidatePainted_ = GetClientRect(window, &client) &&
                    owner->candidateRenderer_->paintOpaque(dc, client);
            } catch (...) {
                owner->opaqueCandidatePainted_ = false;
            }
        }
        EndPaint(window, &paint);
        return 0;
    }
    if (message == WM_DPICHANGED && owner && window == owner->candidateWindow_ && owner->candidateRenderer_) {
        if ((GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_LAYERED) != 0) {
            const UINT dpi = GetDpiForWindow(window);
            owner->candidateRenderer_->repaint(window, static_cast<int>(dpi == 0 ? 96 : dpi));
        } else {
            InvalidateRect(window, nullptr, FALSE);
        }
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

// 输入法在 Windows 上没有语言栏按钮，安装完成后如果没记住开始菜单入口，
// 就再也找不到设置程序；在候选窗上补一个右键菜单作为常驻入口。
void XMJDTip::showContextMenu(HWND window) {
    // In elevated applications the channel is for text input only. Do not open
    // Settings or a user-controlled folder using the game's token.
    if (!pipe_.connected() || pipe_.inputOnly()) return;
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(menu, MF_STRING, kMenuSettings, L"星猫键道设置…");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuUserData, L"打开用户目录");
    AppendMenuW(menu, MF_STRING, kMenuLogs, L"打开日志目录");
    POINT point{};
    if (!GetCursorPos(&point)) {
        DestroyMenu(menu);
        return;
    }
    const UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
                                        point.x, point.y, 0, window, nullptr);
    DestroyMenu(menu);
    PostMessageW(window, WM_NULL, 0, 0);
    if (command == 0) return;

    const auto root = xmjd::win::dataRoot();
    if (command == kMenuSettings) {
        std::wstring executable;
        if (!readHostExecutable(executable)) return;
        const std::filesystem::path directory = std::filesystem::path(executable).parent_path();
        const std::filesystem::path settings = directory / L"XMJDSettings.exe";
        std::error_code error;
        if (!std::filesystem::is_regular_file(settings, error)) return;
        ShellExecuteW(nullptr, L"open", settings.c_str(), nullptr, directory.c_str(), SW_SHOWNORMAL);
    } else if (command == kMenuUserData) {
        if (!root.empty()) ShellExecuteW(nullptr, L"open", root.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    } else if (command == kMenuLogs) {
        if (!root.empty()) {
            ShellExecuteW(nullptr, L"open", (root / L"Logs").c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
    }
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
    shown_ = show != FALSE && owner_->uiElementShow_ ? TRUE : FALSE;
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
