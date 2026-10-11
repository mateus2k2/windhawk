// ==WindhawkMod==
// @id              groupy-tray-icon
// @name            Groupy 2: tray icon
// @description     Adds a notification area icon for Groupy 2 that opens its settings.
// @version         0.3
// @author          mateu
// @include         GroupyCtrl.exe
// @compilerOptions -lshell32
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Groupy 2: tray icon

Groupy 2 has no notification area icon. This mod runs inside `GroupyCtrl.exe`
and adds one, so the icon appears and disappears together with Groupy.

- Left click: open Groupy's settings, or close them if they are already open.
- Right click: menu with the same action, plus "Exit Groupy".

Groupy runs elevated, so Windhawk has to run as administrator (or be installed
as a service) for this mod to load.
*/
// ==/WindhawkModReadme==

#include <windows.h>

#include <shellapi.h>

constexpr WCHAR kWindowClass[] = L"GroupyTrayIconMod";
constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT_PTR kRetryTimerId = 1;
constexpr UINT kRetryIntervalMs = 2000;
constexpr UINT kMenuOpenSettings = 1;
constexpr UINT kMenuExitGroupy = 2;

HANDLE g_thread;
HWND g_hwnd;
HICON g_icon;
ULONGLONG g_lastClickTick;
UINT g_taskbarCreatedMessage;
WCHAR g_configPath[MAX_PATH];

void OpenSettings() {
    ShellExecuteW(nullptr, L"open", g_configPath, nullptr, nullptr,
                  SW_SHOWNORMAL);
}

BOOL CALLBACK FindSettingsWindowProc(HWND hwnd, LPARAM lParam) {
    if (!IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER)) {
        return TRUE;
    }

    DWORD processId = 0;
    GetWindowThreadProcessId(hwnd, &processId);
    HANDLE process =
        OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (!process) {
        return TRUE;
    }

    WCHAR path[MAX_PATH];
    DWORD size = ARRAYSIZE(path);
    bool isSettings = QueryFullProcessImageNameW(process, 0, path, &size) &&
                      _wcsicmp(path, g_configPath) == 0;
    CloseHandle(process);

    if (isSettings) {
        *(HWND*)lParam = hwnd;
        return FALSE;
    }

    return TRUE;
}

// The main window of a running GroupyConfig.exe, or nullptr.
HWND FindSettingsWindow() {
    HWND hwnd = nullptr;
    EnumWindows(FindSettingsWindowProc, (LPARAM)&hwnd);
    return hwnd;
}

// Opens the settings, or closes them if they are already open.
void ToggleSettings() {
    if (HWND settings = FindSettingsWindow()) {
        PostMessageW(settings, WM_CLOSE, 0, 0);
    } else {
        OpenSettings();
    }
}

bool AddTrayIcon(HWND hwnd) {
    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = kTrayMessage;
    nid.hIcon = g_icon;
    wcscpy_s(nid.szTip, L"Groupy");
    return Shell_NotifyIconW(NIM_ADD, &nid);
}

void RemoveTrayIcon(HWND hwnd) {
    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

// The notification area may not exist yet when Groupy starts at logon, so
// keep retrying until the icon is added.
void AddTrayIconWithRetry(HWND hwnd) {
    if (AddTrayIcon(hwnd)) {
        KillTimer(hwnd, kRetryTimerId);
    } else {
        SetTimer(hwnd, kRetryTimerId, kRetryIntervalMs, nullptr);
    }
}

// Asks Groupy to shut down the same way its own "Turn off Groupy" switch
// appears to: by broadcasting its registered unload message. Groupy has to do
// this itself; killing its process would leave grouped windows shifted and
// inactive tabs hidden.
void ExitGroupy(HWND hwnd) {
    int choice = MessageBoxW(
        hwnd,
        L"Turn Groupy off?\n\nAll groups will be split back into separate "
        L"windows.",
        L"Groupy", MB_OKCANCEL | MB_ICONWARNING | MB_SETFOREGROUND | MB_TOPMOST);
    if (choice != IDOK) {
        return;
    }

    UINT unloadMessage = RegisterWindowMessageW(L"GROUPY_UNLOAD");
    if (unloadMessage) {
        PostMessageW(HWND_BROADCAST, unloadMessage, 0, 0);
    }
}

void ShowTrayMenu(HWND hwnd) {
    HMENU menu = CreatePopupMenu();
    if (!menu) {
        return;
    }

    AppendMenuW(menu, MF_STRING, kMenuOpenSettings,
                FindSettingsWindow() ? L"Close settings" : L"Open settings");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuExitGroupy, L"Exit Groupy");
    SetMenuDefaultItem(menu, kMenuOpenSettings, FALSE);

    POINT pt;
    GetCursorPos(&pt);

    // Required so the menu closes when clicking elsewhere.
    SetForegroundWindow(hwnd);
    UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y,
                              0, hwnd, nullptr);
    PostMessageW(hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);

    if (cmd == kMenuOpenSettings) {
        ToggleSettings();
    } else if (cmd == kMenuExitGroupy) {
        ExitGroupy(hwnd);
    }
}

LRESULT CALLBACK TrayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE:
            // Groupy runs elevated and the taskbar doesn't, so Windows would
            // otherwise drop the messages the taskbar sends to this window.
            ChangeWindowMessageFilterEx(hwnd, kTrayMessage, MSGFLT_ALLOW,
                                        nullptr);
            if (g_taskbarCreatedMessage) {
                ChangeWindowMessageFilterEx(hwnd, g_taskbarCreatedMessage,
                                            MSGFLT_ALLOW, nullptr);
            }
            AddTrayIconWithRetry(hwnd);
            return 0;

        case WM_TIMER:
            if (wParam == kRetryTimerId) {
                AddTrayIconWithRetry(hwnd);
            }
            return 0;

        case kTrayMessage:
            switch (LOWORD(lParam)) {
                case WM_LBUTTONUP: {
                    // A double click sends two of these; without this the
                    // second would undo the first.
                    ULONGLONG now = GetTickCount64();
                    if (now - g_lastClickTick > GetDoubleClickTime()) {
                        g_lastClickTick = now;
                        ToggleSettings();
                    }
                    break;
                }
                case WM_RBUTTONUP:
                    ShowTrayMenu(hwnd);
                    break;
            }
            return 0;

        case WM_DESTROY:
            KillTimer(hwnd, kRetryTimerId);
            RemoveTrayIcon(hwnd);
            PostQuitMessage(0);
            return 0;
    }

    // Explorer restarted: the icon is gone and has to be added again.
    if (msg == g_taskbarCreatedMessage && g_taskbarCreatedMessage) {
        AddTrayIconWithRetry(hwnd);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

DWORD WINAPI TrayThread(LPVOID) {
    HINSTANCE instance = GetModuleHandleW(nullptr);

    WNDCLASSW wc = {};
    wc.lpfnWndProc = TrayWndProc;
    wc.hInstance = instance;
    wc.lpszClassName = kWindowClass;
    if (!RegisterClassW(&wc)) {
        Wh_Log(L"RegisterClass failed: %u", GetLastError());
        return 0;
    }

    // A hidden top-level window rather than a message-only one, because
    // message-only windows don't receive the TaskbarCreated broadcast.
    g_hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, kWindowClass, L"", WS_POPUP, 0,
                             0, 0, 0, nullptr, nullptr, instance, nullptr);
    if (!g_hwnd) {
        Wh_Log(L"CreateWindow failed: %u", GetLastError());
        UnregisterClassW(kWindowClass, instance);
        return 0;
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    g_hwnd = nullptr;
    UnregisterClassW(kWindowClass, instance);
    return 0;
}

BOOL Wh_ModInit() {
    // GroupyConfig.exe sits next to GroupyCtrl.exe.
    DWORD len =
        GetModuleFileNameW(nullptr, g_configPath, ARRAYSIZE(g_configPath));
    if (!len || len >= ARRAYSIZE(g_configPath)) {
        Wh_Log(L"GetModuleFileName failed");
        return FALSE;
    }

    WCHAR* slash = wcsrchr(g_configPath, L'\\');
    if (!slash) {
        return FALSE;
    }
    slash[1] = L'\0';
    if (wcscat_s(g_configPath, L"GroupyConfig.exe") != 0) {
        return FALSE;
    }
    Wh_Log(L"Settings path: %s", g_configPath);

    ExtractIconExW(g_configPath, 0, nullptr, &g_icon, 1);
    if (!g_icon) {
        g_icon = LoadIconW(nullptr, IDI_APPLICATION);
    }

    g_taskbarCreatedMessage = RegisterWindowMessageW(L"TaskbarCreated");

    g_thread = CreateThread(nullptr, 0, TrayThread, nullptr, 0, nullptr);
    if (!g_thread) {
        Wh_Log(L"CreateThread failed: %u", GetLastError());
        return FALSE;
    }

    return TRUE;
}

void Wh_ModUninit() {
    if (g_thread) {
        // The window is created on the tray thread; wait briefly in case
        // unload races with startup.
        for (int i = 0; i < 50 && !g_hwnd; i++) {
            if (WaitForSingleObject(g_thread, 20) == WAIT_OBJECT_0) {
                break;
            }
        }

        if (g_hwnd) {
            PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
        }

        WaitForSingleObject(g_thread, INFINITE);
        CloseHandle(g_thread);
        g_thread = nullptr;
    }

    if (g_icon) {
        DestroyIcon(g_icon);
        g_icon = nullptr;
    }
}
