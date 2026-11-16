#include <windows.h>
#include <richedit.h>

#include <algorithm>
#include <string>
#include <string_view>

namespace {
constexpr wchar_t kWindowClass[] = L"XMJDEditorHarness";
constexpr int kEditId = 1001;
constexpr int kRichEditId = 1002;
constexpr int kEditLabelId = 1003;
constexpr int kRichEditLabelId = 1004;

void layout(HWND window, int width, int height) {
    constexpr int padding = 12;
    constexpr int labelHeight = 24;
    const int fieldWidth = std::max(0, width - 2 * padding);
    const int fieldHeight = std::max(0, (height - 5 * padding - 2 * labelHeight) / 2);
    int top = padding;
    MoveWindow(GetDlgItem(window, kEditLabelId), padding, top, fieldWidth, labelHeight, TRUE);
    top += labelHeight;
    MoveWindow(GetDlgItem(window, kEditId), padding, top, fieldWidth, fieldHeight, TRUE);
    top += fieldHeight + padding;
    MoveWindow(GetDlgItem(window, kRichEditLabelId), padding, top, fieldWidth, labelHeight, TRUE);
    top += labelHeight;
    MoveWindow(GetDlgItem(window, kRichEditId), padding, top, fieldWidth, fieldHeight, TRUE);
}

LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_CREATE) {
        const HINSTANCE instance = GetModuleHandleW(nullptr);
        constexpr DWORD style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL |
                                 ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN;
        const HWND label = CreateWindowExW(0, L"STATIC", L"标准 Edit（Ctrl+Tab 切换字段）",
                                           WS_CHILD | WS_VISIBLE, 0, 0, 1, 1, window,
                                           reinterpret_cast<HMENU>(kEditLabelId), instance, nullptr);
        const HWND edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", style,
                                          0, 0, 1, 1, window, reinterpret_cast<HMENU>(kEditId), instance, nullptr);
        const HWND richLabel = CreateWindowExW(0, L"STATIC", L"RichEdit（检查组字与候选是否跨字段）",
                                               WS_CHILD | WS_VISIBLE, 0, 0, 1, 1, window,
                                               reinterpret_cast<HMENU>(kRichEditLabelId), instance, nullptr);
        const HWND richEdit = CreateWindowExW(WS_EX_CLIENTEDGE, MSFTEDIT_CLASS, L"", style,
                                              0, 0, 1, 1, window, reinterpret_cast<HMENU>(kRichEditId), instance, nullptr);
        if (!label || !edit || !richLabel || !richEdit) return -1;
        return 0;
    }
    if (message == WM_SIZE) {
        layout(window, static_cast<int>(LOWORD(lParam)), static_cast<int>(HIWORD(lParam)));
        return 0;
    }
    if (message == WM_SETFOCUS) {
        SetFocus(GetDlgItem(window, kEditId));
        return 0;
    }
    if (message == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

bool hasText(HWND edit, std::wstring_view expected) {
    const int length = GetWindowTextLengthW(edit);
    std::wstring actual(static_cast<std::size_t>(length) + 1, L'\0');
    const int copied = GetWindowTextW(edit, actual.data(), static_cast<int>(actual.size()));
    actual.resize(static_cast<std::size_t>(copied));
    return actual == expected;
}

bool runSmoke(HWND window) {
    const HWND edit = GetDlgItem(window, kEditId);
    const HWND richEdit = GetDlgItem(window, kRichEditId);
    constexpr wchar_t first[] = L"星猫 Edit A\U0001F600";
    constexpr wchar_t second[] = L"星猫 RichEdit B\U0001F642";
    if (!SetWindowTextW(edit, first) || !SetWindowTextW(richEdit, second)) return false;
    if (!hasText(edit, first) || !hasText(richEdit, second)) return false;
    SendMessageW(edit, EM_SETSEL, 0, -1);
    SendMessageW(edit, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(L"另一个字段"));
    if (!hasText(edit, L"另一个字段") || !hasText(richEdit, second)) return false;
    SendMessageW(richEdit, EM_SETSEL, 0, -1);
    SendMessageW(richEdit, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(L"独立 RichEdit"));
    return hasText(edit, L"另一个字段") && hasText(richEdit, L"独立 RichEdit");
}
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR commandLine, int showCommand) {
    const HMODULE richLibrary = LoadLibraryExW(L"Msftedit.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!richLibrary) return 1;
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = windowProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_IBEAM);
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    windowClass.lpszClassName = kWindowClass;
    if (!RegisterClassW(&windowClass)) {
        FreeLibrary(richLibrary);
        return 2;
    }
    constexpr const wchar_t* title = sizeof(void*) == 8 ? L"XMJD Editor Harness (x64)" : L"XMJD Editor Harness (x86)";
    const HWND window = CreateWindowExW(WS_EX_CONTROLPARENT, kWindowClass, title,
                                        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                        680, 540, nullptr, nullptr, instance, nullptr);
    if (!window) {
        UnregisterClassW(kWindowClass, instance);
        FreeLibrary(richLibrary);
        return 3;
    }
    if (commandLine && std::wstring_view(commandLine) == L"--smoke") {
        const bool passed = runSmoke(window);
        DestroyWindow(window);
        UnregisterClassW(kWindowClass, instance);
        FreeLibrary(richLibrary);
        return passed ? 0 : 4;
    }
    ShowWindow(window, showCommand);
    UpdateWindow(window);
    SetFocus(GetDlgItem(window, kEditId));
    MSG message{};
    int status = 0;
    while ((status = GetMessageW(&message, nullptr, 0, 0)) > 0) {
        if (message.message == WM_KEYDOWN && message.wParam == VK_TAB &&
            (GetKeyState(VK_CONTROL) & 0x8000) != 0) {
            const HWND other = GetFocus() == GetDlgItem(window, kEditId)
                ? GetDlgItem(window, kRichEditId) : GetDlgItem(window, kEditId);
            SetFocus(other);
            continue;
        }
        if (IsDialogMessageW(window, &message)) continue;
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    UnregisterClassW(kWindowClass, instance);
    FreeLibrary(richLibrary);
    return status == -1 ? 5 : static_cast<int>(message.wParam);
}
