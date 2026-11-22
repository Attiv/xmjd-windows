#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <msctf.h>
#include <ctfutb.h>
#include <oleauto.h>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "../Common/Pipe.h"
#include "ShiftTap.h"

namespace xmjd::tip {
enum class CandidatePresentation;

class CandidateRenderer;

struct CandidateValue {
    std::wstring label;
    std::wstring text;
    std::wstring comment;
};

struct CandidateSnapshot {
    std::vector<CandidateValue> values;
    UINT selected = 0;
    std::uint64_t revision = 0;
    std::uint64_t epoch = 0;
    UINT page = 0;
    bool lastPage = true;
};

enum class EditAction { ProcessKey, SelectCandidate, FinalizeRaw, FinalizeConverted, Cancel, CancelComposition, RecoverComposition, UpdateCandidatePosition, RefreshCandidates };

class XMJDTip;
class LangBarItem;

class CandidateElement final : public ITfCandidateListUIElement {
public:
    CandidateElement(XMJDTip* owner, ITfContext* context);
    ~CandidateElement();
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;
    HRESULT STDMETHODCALLTYPE GetDescription(BSTR* description) override;
    HRESULT STDMETHODCALLTYPE GetGUID(GUID* guid) override;
    HRESULT STDMETHODCALLTYPE Show(BOOL show) override;
    HRESULT STDMETHODCALLTYPE IsShown(BOOL* shown) override;
    HRESULT STDMETHODCALLTYPE GetUpdatedFlags(DWORD* flags) override;
    HRESULT STDMETHODCALLTYPE GetDocumentMgr(ITfDocumentMgr** documentManager) override;
    HRESULT STDMETHODCALLTYPE GetCount(UINT* count) override;
    HRESULT STDMETHODCALLTYPE GetSelection(UINT* index) override;
    HRESULT STDMETHODCALLTYPE GetString(UINT index, BSTR* text) override;
    HRESULT STDMETHODCALLTYPE GetPageIndex(UINT* indexes, UINT size, UINT* pageCount) override;
    HRESULT STDMETHODCALLTYPE SetPageIndex(UINT* indexes, UINT size) override;
    HRESULT STDMETHODCALLTYPE GetCurrentPage(UINT* page) override;
    void update(const xmjd::Snapshot& snapshot);
    const CandidateSnapshot& snapshot() const { return snapshot_; }
    bool isShown() const { return shown_ != FALSE; }
    void detach() noexcept;
private:
    std::atomic<ULONG> refs_{1};
    XMJDTip* owner_;
    ITfContext* context_;
    CandidateSnapshot snapshot_;
    BOOL shown_ = FALSE;
    DWORD updatedFlags_ = TF_CLUIE_COUNT | TF_CLUIE_SELECTION | TF_CLUIE_STRING | TF_CLUIE_PAGEINDEX | TF_CLUIE_CURRENTPAGE;
};

class EditSession final : public ITfEditSession {
public:
    EditSession(XMJDTip* owner, EditAction action, UINT keycode = 0, UINT modifiers = 0,
                UINT candidateIndex = 0, std::uint64_t revision = 0, std::uint64_t epoch = 0);
    ~EditSession();
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;
    HRESULT STDMETHODCALLTYPE DoEditSession(TfEditCookie cookie) override;
    bool handled() const { return handled_; }
    bool consumed() const { return consumed_; }
private:
    friend class XMJDTip;
    std::atomic<ULONG> refs_{1};
    XMJDTip* owner_;
    ITfContext* context_;
    ITfComposition* composition_ = nullptr;
    EditAction action_;
    UINT keycode_;
    UINT modifiers_;
    UINT candidateIndex_;
    std::uint64_t revision_;
    std::uint64_t epoch_;
    bool handled_ = false;
    bool consumed_ = false;
};

class XMJDTip final : public ITfTextInputProcessorEx,
                      public ITfKeyEventSink,
                      public ITfThreadMgrEventSink,
                      public ITfCompositionSink,
                      public ITfInputProcessorProfileActivationSink {
public:
    XMJDTip();
    ~XMJDTip();
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;
    HRESULT STDMETHODCALLTYPE Activate(ITfThreadMgr* manager, TfClientId clientId) override;
    HRESULT STDMETHODCALLTYPE ActivateEx(ITfThreadMgr* manager, TfClientId clientId, DWORD flags) override;
    HRESULT STDMETHODCALLTYPE Deactivate() override;
    HRESULT STDMETHODCALLTYPE OnSetFocus(BOOL foreground) override;
    HRESULT STDMETHODCALLTYPE OnInitDocumentMgr(ITfDocumentMgr* document) override;
    HRESULT STDMETHODCALLTYPE OnUninitDocumentMgr(ITfDocumentMgr* document) override;
    HRESULT STDMETHODCALLTYPE OnSetFocus(ITfDocumentMgr* focused, ITfDocumentMgr* previous) override;
    HRESULT STDMETHODCALLTYPE OnPushContext(ITfContext* context) override;
    HRESULT STDMETHODCALLTYPE OnPopContext(ITfContext* context) override;
    HRESULT STDMETHODCALLTYPE OnTestKeyDown(ITfContext* context, WPARAM key, LPARAM keyData, BOOL* eaten) override;
    HRESULT STDMETHODCALLTYPE OnTestKeyUp(ITfContext* context, WPARAM key, LPARAM keyData, BOOL* eaten) override;
    HRESULT STDMETHODCALLTYPE OnKeyDown(ITfContext* context, WPARAM key, LPARAM keyData, BOOL* eaten) override;
    HRESULT STDMETHODCALLTYPE OnKeyUp(ITfContext* context, WPARAM key, LPARAM keyData, BOOL* eaten) override;
    HRESULT STDMETHODCALLTYPE OnPreservedKey(ITfContext* context, REFGUID key, BOOL* eaten) override;
    HRESULT STDMETHODCALLTYPE OnCompositionTerminated(TfEditCookie cookie, ITfComposition* composition) override;
    HRESULT STDMETHODCALLTYPE OnActivated(DWORD profileType, LANGID language, REFCLSID clsid, REFGUID category,
                                           REFGUID profile, HKL layout, DWORD flags) override;

    HRESULT process(EditSession* edit, TfEditCookie cookie);
    void candidateVisibilityChanged(bool shown);
    void candidateClicked(UINT index, std::uint64_t revision);

private:
    friend class CandidateElement;
    friend class EditSession;
    void deactivate() noexcept;
    void resetForFocusChange();
    HRESULT getFocusedContext(ITfContext** context, ITfDocumentMgr** documentManager) const;
    bool isCurrentContext(ITfContext* context) const;
    bool isValidEdit(const EditSession* edit, bool requireFocus = true) const;
    bool ensureContext(ITfContext* context);
    void invalidateSession(bool closePipe) noexcept;
    bool transact(xmjd::Operation operation, xmjd::Request& request, xmjd::Response& response);
    bool createEngineSession();
    void failEngineSession(EditSession* edit, TfEditCookie cookie);
    bool recoverComposition(EditSession* edit, TfEditCookie cookie);
    void retryEngineSession();
    void scheduleEngineRetry();
    void cancelEngineRetry() noexcept;
    void scheduleCorrectionRefresh(bool pending);
    void pollCorrection();
    bool applySnapshot(EditSession* edit, TfEditCookie cookie, const xmjd::Snapshot& snapshot);
    bool setCompositionText(EditSession* edit, TfEditCookie cookie, std::wstring_view text, UINT caret);
    bool commitText(EditSession* edit, TfEditCookie cookie, std::wstring_view text);
    bool endComposition(EditSession* edit, TfEditCookie cookie, bool eraseText);
    bool startComposition(EditSession* edit, TfEditCookie cookie);
    void updateCandidateUi(const xmjd::Snapshot& snapshot, TfEditCookie cookie);
    // 用引擎快照同步 TSF 语言栏状态。
    void reportImeMode(bool asciiMode);
    // 设置入口；Windows 应用兼容性由 TSF 类别注册声明。
    void createLangBarItem();
    void destroyLangBarItem();
    void closeCandidateUi() noexcept;
    bool positionCandidateWindow(TfEditCookie cookie);
    bool showCandidateWindow(bool show);
    bool ensurePopupClass();
    bool ensureCandidateWindow(HWND viewWindow, CandidatePresentation presentation);
    void destroyCandidateWindow() noexcept;
    void showContextMenu(HWND window);
    bool processCandidateSelection(EditSession* edit, TfEditCookie cookie);
    UINT rimeModifiers() const;
    bool shouldTestKey(UINT virtualKey, bool keyUp) const;
    bool translateKey(UINT virtualKey, LPARAM keyData, UINT& keycode, UINT& modifiers) const;
    HRESULT handleKey(ITfContext* context, WPARAM key, LPARAM keyData, bool keyUp, BOOL* eaten);
    HRESULT requestEdit(ITfContext* context, EditSession* edit, DWORD flags, HRESULT* sessionResult);
    static LRESULT CALLBACK popupWndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

    std::atomic<ULONG> refs_{1};
    ITfThreadMgr* threadManager_ = nullptr;
    ITfLangBarItemMgr* langBarManager_ = nullptr;
    LangBarItem* langBarItem_ = nullptr;
    ITfKeystrokeMgr* keystrokeManager_ = nullptr;
    ITfUIElementMgr* uiElementManager_ = nullptr;
    ITfSource* threadSource_ = nullptr;
    ITfDocumentMgr* documentManager_ = nullptr;
    ITfContext* context_ = nullptr;
    ITfComposition* composition_ = nullptr;
    CandidateElement* candidateElement_ = nullptr;
    xmjd::win::PipeClient pipe_;
    TfClientId clientId_ = TF_CLIENTID_NULL;
    bool asciiMode_ = false;          // 引擎当前的英文模式
    DWORD uiElementId_ = TF_INVALID_UIELEMENTID;
    DWORD activationFlags_ = 0;
    DWORD profileSinkCookie_ = TF_INVALID_COOKIE;
    DWORD threadSinkCookie_ = TF_INVALID_COOKIE;
    std::uint64_t contextEpoch_ = 0;
    std::uint64_t sequence_ = 0;
    std::uint64_t sessionId_ = 0;
    std::uint64_t candidateRevision_ = 0;
    std::wstring acknowledgedRaw_;
    bool engineRetryScheduled_ = false;
    bool correctionAvailable_ = false;
    ULONGLONG correctionDeadline_ = 0;
    HWND engineRetryWindow_ = nullptr;
    HWND candidateWindow_ = nullptr;
    bool opaqueCandidatePainted_ = false;
    bool candidateCompositionFailed_ = false;
    bool allowCandidateAppearance_ = true;
    std::unique_ptr<CandidateRenderer> candidateRenderer_;
    bool activated_ = false;
    bool foreground_ = false;
    bool resettingContext_ = false;
    bool processingEdit_ = false;
    bool keySinkAdvised_ = false;
    bool hasEngineSession_ = false;
    bool compositionTerminated_ = false;
    bool contextUnavailable_ = false;
    bool capsKeyDown_ = false;
    ShiftTap shiftTap_;
    UINT modifierKeysDown_ = 0;
    bool uiElementShow_ = false;
    bool uiElementAttempted_ = false;
};

} // namespace xmjd::tip
