#pragma once

#include <windows.h>
#include <array>

namespace xmjd::tip {

inline HWND candidateOwnerRoot(HWND window) noexcept {
    const HWND root = window ? GetAncestor(window, GA_ROOT) : nullptr;
    if (!root || root == GetDesktopWindow() ||
        (GetWindowLongPtrW(root, GWL_STYLE) & WS_CHILD) != 0) return nullptr;
    return root;
}

inline std::array<HWND, 3> candidateOwnerChoices(HWND view, HWND focus, HWND foreground) noexcept {
    const HWND viewRoot = candidateOwnerRoot(view);
    // Other HWNDs are just fallbacks; never use an unrelated foreground app.
    const auto localRoot = [](HWND window) {
        const HWND root = candidateOwnerRoot(window);
        DWORD process = 0;
        if (root) GetWindowThreadProcessId(root, &process);
        return process == GetCurrentProcessId() ? root : nullptr;
    };
    const HWND focused = localRoot(focus);
    const HWND active = localRoot(foreground);
    return {viewRoot, focused != viewRoot ? focused : nullptr,
            active != viewRoot && active != focused ? active : nullptr};
}

enum class CandidatePresentation { Layered, Composition, Opaque };

inline HWND createCandidatePopup(LPCWSTR windowClass, HINSTANCE instance, HWND owner, void* context,
                                 CandidatePresentation presentation = CandidatePresentation::Layered) noexcept {
    // Establish ownership when creating the popup, not on a detached window.
    const DWORD surface = presentation == CandidatePresentation::Layered ? WS_EX_LAYERED :
                          presentation == CandidatePresentation::Composition ? WS_EX_NOREDIRECTIONBITMAP : 0;
    const DWORD style = WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | surface | (owner ? 0 : WS_EX_TOPMOST);
    return CreateWindowExW(style, windowClass, L"", WS_POPUP,
                           0, 0, 1, 1, owner, nullptr, instance, context);
}

inline bool usableCandidateCaret(const RECT& rect) noexcept {
    // A zero-width insertion caret is valid; the all-zero/no-layout rect is not.
    return rect.right >= rect.left && rect.bottom > rect.top;
}

} // namespace xmjd::tip
