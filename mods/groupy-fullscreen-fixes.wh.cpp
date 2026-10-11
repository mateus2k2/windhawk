// ==WindhawkMod==
// @id              groupy-fullscreen-fixes
// @name            Groupy 2: full screen fixes
// @description     Fixes two Groupy 2 glitches when a grouped browser window leaves full screen: the tab bar staying transparent, and the window coming back non-maximized.
// @version         0.1
// @author          mateu
// @include         firefox.exe
// @include         chrome.exe
// @include         msedge.exe
// @compilerOptions -lcomctl32
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Groupy 2: full screen fixes

Watches the browser's own windows and acts when a Groupy-grouped window leaves
full screen:

- **Transparent tab bar.** Groupy shows its tab bar again but doesn't paint it
  until the mouse hovers it or the window loses focus. The mod announces a
  "title changed" event for the window, which makes Groupy repaint the bar.
- **Lost maximized state.** A window that Groupy maximized can come back at its
  restored size, which Groupy then applies to every tab in the group. The mod
  maximizes it again.

Firefox only:

- **Stale maximized state.** Groupy maximizes and restores grouped windows
  without Firefox noticing, so Firefox can draw a maximized window as if it were
  a normal one, pushing its tabs further under Groupy's bar. The mod tells
  Firefox to re-read the window's state when Groupy changes it and when a tab
  is switched to.

- **Tab bar covering Firefox's tabs.** Groupy's bar is a few pixels taller than
  the space Groupy reserves for it. For grouped, maximized windows the mod
  starts Firefox's content at the bottom of the bar. Ungrouped windows are not
  changed.

To cover another program, add an `@include` line for it.
*/
// ==/WindhawkModReadme==

#include <windhawk_utils.h>

#include <windows.h>

#include <commctrl.h>

#include <unordered_map>
#include <unordered_set>
#include <vector>

// How long after leaving full screen a non-maximized window is still treated
// as the bug rather than as the user restoring the window.
constexpr ULONGLONG kMaximizeFixWindowMs = 1000;

// When to nudge Groupy into repainting its tab bar, counted from the moment
// the window leaves full screen. Repeated because it's unclear how soon
// Groupy is ready; extra nudges are harmless.
constexpr ULONGLONG kRepaintDelaysMs[] = {200, 700, 1500};

// Groupy's tab bar is a few pixels taller than the space Groupy reserves for
// it, so it covers the top of a grouped window's content. Larger gaps than
// this are not that overlap and are left alone.
constexpr int kMaxBarOverlapFix = 16;

// After Groupy changes a window's state, Firefox is resynced right away and
// once more after this delay, by which time Groupy's bar is in its final place.
constexpr ULONGLONG kResyncRetryDelayMs = 300;

struct WindowState {
    bool seen = false;
    bool lastMaximized = false;
    bool fullScreen = false;
    bool fullScreenMaximized = false;
    ULONGLONG leftFullScreenTick = 0;
};

enum class PendingKind {
    // Nudge Groupy into repainting its tab bar.
    RepaintBar,
    // Make Firefox re-read the window's frame state.
    ResyncFrame,
};

struct PendingAction {
    PendingKind kind;
    HWND hwnd;
    ULONGLONG dueTick;
};

HANDLE g_thread;
DWORD g_threadId;
bool g_isFirefox;

// Only touched from the hook thread.
std::unordered_map<HWND, WindowState> g_windows;
std::unordered_set<HWND> g_subclassedWindows;
std::vector<PendingAction> g_pendingActions;
UINT_PTR g_timerId;

// Firefox keeps its own record of whether a window is maximized and only
// refreshes it when it is told the frame changed. Groupy maximizes and restores
// grouped windows without that notification, so Firefox can go on drawing a
// maximized window as if it were a normal one (its tabs end up further under
// Groupy's bar) and later restore it to the wrong state. Sending the
// notification makes Firefox re-read the real state. It also makes the window
// recalculate its client area, which applies the tab bar overlap fix below.
void ResyncFirefoxFrameState(HWND hwnd) {
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER |
                     SWP_NOACTIVATE | SWP_FRAMECHANGED | SWP_ASYNCWINDOWPOS);
}

void SchedulePendingAction(PendingKind kind, HWND hwnd, ULONGLONG delay) {
    g_pendingActions.push_back({kind, hwnd, GetTickCount64() + delay});

    if (!g_timerId) {
        g_timerId = SetTimer(nullptr, 0, 50, nullptr);
    }
}

void ScheduleRepaints(HWND hwnd) {
    for (ULONGLONG delay : kRepaintDelaysMs) {
        SchedulePendingAction(PendingKind::RepaintBar, hwnd, delay);
    }
}

void ProcessPendingActions() {
    ULONGLONG now = GetTickCount64();

    for (auto it = g_pendingActions.begin(); it != g_pendingActions.end();) {
        if (it->dueTick > now) {
            ++it;
            continue;
        }

        if (IsWindow(it->hwnd)) {
            switch (it->kind) {
                case PendingKind::RepaintBar:
                    // Groupy redraws the tab when the window's title changes.
                    NotifyWinEvent(EVENT_OBJECT_NAMECHANGE, it->hwnd,
                                   OBJID_WINDOW, CHILDID_SELF);
                    break;
                case PendingKind::ResyncFrame:
                    ResyncFirefoxFrameState(it->hwnd);
                    break;
            }
        }
        it = g_pendingActions.erase(it);
    }

    if (g_pendingActions.empty() && g_timerId) {
        KillTimer(nullptr, g_timerId);
        g_timerId = 0;
    }
}

// The bottom edge of Groupy's tab bar for a grouped, maximized window, in
// screen coordinates. Fails for windows where the bar doesn't sit above the
// content: ungrouped, not maximized, or full screen.
bool GetGroupyBarBottom(HWND hwnd, int* barBottom) {
    LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    if (!(style & WS_MAXIMIZE) || (style & WS_CAPTION) != WS_CAPTION) {
        return false;
    }

    HWND bar = (HWND)GetPropW(hwnd, L"GP_LINK");
    RECT barRect;
    if (!bar || !GetWindowRect(bar, &barRect)) {
        return false;
    }

    *barBottom = barRect.bottom;
    return true;
}

// Starts the client area of a grouped Firefox window at the bottom of Groupy's
// tab bar, so the bar no longer covers the top few pixels of Firefox's tabs.
// The strip that stops being client area is hidden under the bar. Ungrouped
// windows are left as Firefox laid them out.
LRESULT CALLBACK FirefoxSubclassProc(HWND hwnd,
                                     UINT msg,
                                     WPARAM wParam,
                                     LPARAM lParam,
                                     DWORD_PTR) {
    if (msg != WM_NCCALCSIZE) {
        return DefSubclassProc(hwnd, msg, wParam, lParam);
    }

    LRESULT result = DefSubclassProc(hwnd, msg, wParam, lParam);

    RECT* clientRect = wParam ? &((NCCALCSIZE_PARAMS*)lParam)->rgrc[0]
                              : (RECT*)lParam;
    int barBottom;
    if (GetGroupyBarBottom(hwnd, &barBottom)) {
        int overlap = barBottom - clientRect->top;
        if (overlap > 0 && overlap <= kMaxBarOverlapFix &&
            clientRect->bottom - clientRect->top > overlap) {
            clientRect->top += overlap;
        }
    }

    return result;
}

void EnsureFirefoxSubclassed(HWND hwnd) {
    if (g_subclassedWindows.contains(hwnd)) {
        return;
    }

    WCHAR className[32];
    if (!GetClassNameW(hwnd, className, ARRAYSIZE(className)) ||
        wcscmp(className, L"MozillaWindowClass") != 0) {
        return;
    }

    if (WindhawkUtils::SetWindowSubclassFromAnyThread(hwnd, FirefoxSubclassProc,
                                                      0)) {
        g_subclassedWindows.insert(hwnd);
    }
}

void RemoveFirefoxSubclasses() {
    for (HWND hwnd : g_subclassedWindows) {
        if (IsWindow(hwnd)) {
            WindhawkUtils::RemoveWindowSubclassFromAnyThread(
                hwnd, FirefoxSubclassProc);
            // Let the window go back to its own client area.
            ResyncFirefoxFrameState(hwnd);
        }
    }
    g_subclassedWindows.clear();
}

void CALLBACK WinEventProc(HWINEVENTHOOK,
                           DWORD event,
                           HWND hwnd,
                           LONG idObject,
                           LONG idChild,
                           DWORD,
                           DWORD) {
    if (idObject != OBJID_WINDOW || idChild != CHILDID_SELF || !hwnd) {
        return;
    }

    if (event == EVENT_OBJECT_DESTROY) {
        g_windows.erase(hwnd);
        g_subclassedWindows.erase(hwnd);
        return;
    }

    if (GetAncestor(hwnd, GA_ROOT) != hwnd) {
        return;
    }

    LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    bool hasCaption = (style & WS_CAPTION) == WS_CAPTION;
    bool maximized = (style & WS_MAXIMIZE) != 0;

    // Switching to a tab: make sure Firefox agrees with the window's state.
    if (event == EVENT_SYSTEM_FOREGROUND) {
        if (g_isFirefox && hasCaption && GetPropW(hwnd, L"GP_LINK")) {
            EnsureFirefoxSubclassed(hwnd);
            ResyncFirefoxFrameState(hwnd);
        }
        return;
    }

    if (event != EVENT_OBJECT_LOCATIONCHANGE) {
        return;
    }

    // Groupy changed a grouped window between maximized and restored.
    if (g_isFirefox && hasCaption && GetPropW(hwnd, L"GP_LINK")) {
        EnsureFirefoxSubclassed(hwnd);

        WindowState& state = g_windows[hwnd];
        if (!state.seen || state.lastMaximized != maximized) {
            state.seen = true;
            state.lastMaximized = maximized;
            ResyncFirefoxFrameState(hwnd);
            SchedulePendingAction(PendingKind::ResyncFrame, hwnd,
                                  kResyncRetryDelayMs);
        }
    }

    // A full screen browser window drops its caption. Menus and tooltips have
    // no caption either, but they are never part of a Groupy group.
    if (!hasCaption) {
        if (GetPropW(hwnd, L"GP_LINK")) {
            WindowState& state = g_windows[hwnd];
            // Latch the maximized flag for the whole full screen session: on
            // the way out the window is un-maximized before its caption comes
            // back, and that intermediate state must not clear it.
            if (!state.fullScreen) {
                state.fullScreenMaximized = false;
            }
            state.fullScreen = true;
            state.fullScreenMaximized |= maximized;
            state.leftFullScreenTick = 0;
        }
        return;
    }

    auto it = g_windows.find(hwnd);
    if (it == g_windows.end()) {
        return;
    }

    WindowState& state = it->second;
    ULONGLONG now = GetTickCount64();

    if (state.fullScreen) {
        state.fullScreen = false;
        state.leftFullScreenTick = now;
        Wh_Log(L"%p left full screen", hwnd);
        ScheduleRepaints(hwnd);
    }

    if (!state.leftFullScreenTick) {
        return;
    }

    if (now - state.leftFullScreenTick > kMaximizeFixWindowMs) {
        state.leftFullScreenTick = 0;
        return;
    }

    if (state.fullScreenMaximized && !maximized && !(style & WS_MINIMIZE) &&
        GetPropW(hwnd, L"GP_LINK")) {
        Wh_Log(L"Re-maximizing %p after full screen", hwnd);
        state.leftFullScreenTick = 0;
        ShowWindowAsync(hwnd, SW_MAXIMIZE);
    }
}

DWORD WINAPI HookThread(LPVOID) {
    HWINEVENTHOOK hook = SetWinEventHook(
        EVENT_OBJECT_DESTROY, EVENT_OBJECT_LOCATIONCHANGE, nullptr,
        WinEventProc, GetCurrentProcessId(), 0, WINEVENT_OUTOFCONTEXT);
    if (!hook) {
        Wh_Log(L"SetWinEventHook failed");
        return 0;
    }

    HWINEVENTHOOK foregroundHook = SetWinEventHook(
        EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, WinEventProc,
        GetCurrentProcessId(), 0, WINEVENT_OUTOFCONTEXT);

    // Out-of-context events and the repaint timer are both delivered through
    // this thread's message loop.
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!msg.hwnd && msg.message == WM_TIMER && msg.wParam == g_timerId) {
            ProcessPendingActions();
            continue;
        }

        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    RemoveFirefoxSubclasses();

    if (g_timerId) {
        KillTimer(nullptr, g_timerId);
        g_timerId = 0;
    }
    if (foregroundHook) {
        UnhookWinEvent(foregroundHook);
    }
    UnhookWinEvent(hook);
    return 0;
}

BOOL Wh_ModInit() {
    // Only the main browser process owns top-level windows; skip Firefox
    // content processes and Chromium child processes.
    PCWSTR commandLine = GetCommandLineW();
    if (wcsstr(commandLine, L"-contentproc") || wcsstr(commandLine, L"--type=")) {
        return TRUE;
    }

    WCHAR exePath[MAX_PATH];
    DWORD len = GetModuleFileNameW(nullptr, exePath, ARRAYSIZE(exePath));
    PCWSTR exeName = len ? wcsrchr(exePath, L'\\') : nullptr;
    g_isFirefox = exeName && _wcsicmp(exeName + 1, L"firefox.exe") == 0;

    g_thread = CreateThread(nullptr, 0, HookThread, nullptr, 0, &g_threadId);
    if (!g_thread) {
        Wh_Log(L"CreateThread failed: %u", GetLastError());
        return FALSE;
    }

    return TRUE;
}

void Wh_ModUninit() {
    if (!g_thread) {
        return;
    }

    // The thread's message queue may not exist yet if unload races startup.
    while (WaitForSingleObject(g_thread, 20) == WAIT_TIMEOUT) {
        PostThreadMessageW(g_threadId, WM_QUIT, 0, 0);
    }

    CloseHandle(g_thread);
    g_thread = nullptr;
}
