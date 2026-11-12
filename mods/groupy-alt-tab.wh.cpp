// ==WindhawkMod==
// @id              groupy-alt-tab
// @name            Groupy 2: one Alt+Tab entry per group
// @description     Hides inactive Groupy 2 tabs from the native Alt+Tab switcher, so each group shows only its active tab.
// @version         0.1
// @author          mateu
// @include         explorer.exe
// @architecture    x86-64
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Groupy 2: one Alt+Tab entry per group

Filters the native Windows 11 Alt+Tab list inside explorer.exe so that a Groupy 2
group contributes a single entry (its active tab). The same filter is applied
when Win+Tab (Task View) opens. Only those lists are filtered; the windows
themselves are not changed, so the taskbar and virtual desktops keep behaving
as before.

Group membership is read from the window properties Groupy sets:
a grouped window has `GP_LINK` pointing at the group's tab bar window, and the
tab bar's `GP_WNDLIVE` holds the active tab.
*/
// ==/WindhawkModReadme==

/*
  The explorer.exe hooking approach is taken from the Windhawk mods
  alt-tab-per-monitor by L3r0y and alt-tab-hide-ghost-windows by David Trapp.

  Copyright 2025 David Trapp (CherryDT)
  Copyright 2025 L3r0yThingz

  Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

  The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/

#include <windhawk_utils.h>

#include <atomic>

std::atomic<DWORD> g_threadIdForCreateInstance;
std::atomic<DWORD> g_lastThreadIdForCreateInstance;
std::atomic<ULONGLONG> g_createInstanceTickCount;
constexpr ULONGLONG kDeltaThreshold = 250;

// One bit per multitasking view type (Alt+Tab, Task View, ...) that is
// currently on screen. While any is, the filter stays on, so that Task View
// keeps hiding inactive tabs when it lists the windows of another desktop.
std::atomic<DWORD> g_shownViewTypes;

bool IsInactiveGroupyTab(HWND hwnd) {
    HWND tabBar = (HWND)GetPropW(hwnd, L"GP_LINK");
    if (!tabBar || !IsWindow(tabBar) || !GetPropW(tabBar, L"GP_TAB")) {
        return false;
    }

    HWND live = (HWND)GetPropW(tabBar, L"GP_WNDLIVE");
    if (!live || live == hwnd || !IsWindow(live)) {
        return false;
    }

    // Only trust the tab bar if its active tab points back at it; otherwise the
    // link may be stale and hiding the window would make it unreachable.
    return (HWND)GetPropW(live, L"GP_LINK") == tabBar;
}

void* CWin32ApplicationView_vtable;
void* CWinRTApplicationView_vtable;

using GetNativeWindow_t = HRESULT(WINAPI*)(void* pThis, HWND* windowHandle);
GetNativeWindow_t CWin32ApplicationView_v_GetNativeWindow;
GetNativeWindow_t CWinRTApplicationView_v_GetNativeWindow;

HWND GetWindowHandleFromApplicationView(void* applicationView) {
    HWND hwnd = nullptr;
    void* vtable = *(void**)applicationView;

    if (vtable == CWin32ApplicationView_vtable) {
        CWin32ApplicationView_v_GetNativeWindow(applicationView, &hwnd);
    } else if (vtable == CWinRTApplicationView_vtable) {
        CWinRTApplicationView_v_GetNativeWindow(applicationView, &hwnd);
    }

    return hwnd;
}

using CVirtualDesktop_IsViewVisible_t = HRESULT(WINAPI*)(void* pThis,
                                                         void* applicationView,
                                                         BOOL* isVisible);
CVirtualDesktop_IsViewVisible_t CVirtualDesktop_IsViewVisible_Original;
HRESULT WINAPI CVirtualDesktop_IsViewVisible_Hook(void* pThis,
                                                  void* applicationView,
                                                  BOOL* isVisible) {
    HRESULT ret = CVirtualDesktop_IsViewVisible_Original(pThis, applicationView,
                                                         isVisible);
    if (FAILED(ret) || !*isVisible) {
        return ret;
    }

    DWORD threadId = GetCurrentThreadId();
    if (!g_shownViewTypes && g_threadIdForCreateInstance != threadId) {
        // A focused window might be added shortly after Alt+Tab opens, so keep
        // filtering briefly. Beyond that leave the result alone while no
        // switcher is open: this function is also used by the taskbar.
        if (GetTickCount64() - g_createInstanceTickCount > kDeltaThreshold ||
            g_lastThreadIdForCreateInstance != threadId) {
            return ret;
        }
    }

    HWND hwnd = GetWindowHandleFromApplicationView(applicationView);
    if (hwnd && IsInactiveGroupyTab(hwnd)) {
        Wh_Log(L"Hiding inactive Groupy tab %p", hwnd);
        *isVisible = FALSE;
    }

    return ret;
}

using MultitaskingViewVisibilityService_ViewChanged_t =
    void(WINAPI*)(void* pThis, int viewType);

MultitaskingViewVisibilityService_ViewChanged_t
    MultitaskingViewVisibilityService_ViewShown_Original;
MultitaskingViewVisibilityService_ViewChanged_t
    MultitaskingViewVisibilityService_ViewDismissed_Original;
void WINAPI MultitaskingViewVisibilityService_ViewShown_Hook(void* pThis,
                                                             int viewType) {
    // Without the matching dismissal hook the flag could never be cleared.
    if (MultitaskingViewVisibilityService_ViewDismissed_Original) {
        g_shownViewTypes |= 1u << (viewType & 31);
    }
    Wh_Log(L"View type %d shown", viewType);
    MultitaskingViewVisibilityService_ViewShown_Original(pThis, viewType);
}

void WINAPI MultitaskingViewVisibilityService_ViewDismissed_Hook(void* pThis,
                                                                 int viewType) {
    g_shownViewTypes &= ~(1u << (viewType & 31));
    Wh_Log(L"View type %d dismissed", viewType);
    MultitaskingViewVisibilityService_ViewDismissed_Original(pThis, viewType);
}

using XamlViewHost_CreateInstance_t = HRESULT(WINAPI*)(void* param1,
                                                       void* param2,
                                                       void* param3);

// Marks the current thread as building a switcher, so that IsViewVisible
// filters inactive tabs while the switcher's window list is being collected.
HRESULT CreateInstanceHook(XamlViewHost_CreateInstance_t original,
                           void* param1,
                           void* param2,
                           void* param3) {
    DWORD threadId = GetCurrentThreadId();
    g_threadIdForCreateInstance = threadId;
    g_lastThreadIdForCreateInstance = threadId;
    g_createInstanceTickCount = GetTickCount64();
    HRESULT ret = original(param1, param2, param3);
    g_threadIdForCreateInstance = 0;
    return ret;
}

XamlViewHost_CreateInstance_t XamlAltTabViewHost_CreateInstance_Original;
HRESULT WINAPI XamlAltTabViewHost_CreateInstance_Hook(void* param1,
                                                      void* param2,
                                                      void* param3) {
    return CreateInstanceHook(XamlAltTabViewHost_CreateInstance_Original,
                              param1, param2, param3);
}

// Win+Tab (Task View).
XamlViewHost_CreateInstance_t XamlTaskViewHost_CreateInstance_Original;
HRESULT WINAPI XamlTaskViewHost_CreateInstance_Hook(void* param1,
                                                    void* param2,
                                                    void* param3) {
    return CreateInstanceHook(XamlTaskViewHost_CreateInstance_Original, param1,
                              param2, param3);
}

// Offsets for twinui.pcshell.dll builds whose symbols Microsoft hasn't published
// yet, taken from the ported symbols in windhawk-mod-symbol-cache. Used only
// when HookSymbols fails and the loaded module matches the build exactly.
struct KnownBuildOffsets {
    DWORD timeDateStamp;
    DWORD sizeOfImage;
    DWORD isViewVisible;
    DWORD win32ApplicationViewVtable;
    DWORD win32ApplicationViewGetNativeWindow;
    DWORD winRTApplicationViewVtable;
    DWORD winRTApplicationViewGetNativeWindow;
    DWORD altTabViewHostCreateInstance;
    DWORD taskViewHostCreateInstance;
    DWORD viewShown;
    DWORD viewDismissed;
};

constexpr KnownBuildOffsets kKnownBuilds[] = {
    // 10.0.26100.9974 (KB5132061).
    {1495861785, 10301440, 0x0010CFF0, 0x00774EB0, 0x0033D7D0, 0x00774698,
     0x001FA0A0, 0x00100F9C, 0x00705CA0, 0x002593F0, 0x002549C0},
};

bool HookKnownBuild(HMODULE module) {
    auto base = (BYTE*)module;
    auto dosHeader = (IMAGE_DOS_HEADER*)base;
    auto ntHeaders = (IMAGE_NT_HEADERS*)(base + dosHeader->e_lfanew);

    for (const auto& build : kKnownBuilds) {
        if (ntHeaders->FileHeader.TimeDateStamp != build.timeDateStamp ||
            ntHeaders->OptionalHeader.SizeOfImage != build.sizeOfImage) {
            continue;
        }

        Wh_Log(L"Using built-in offsets for build %u", build.timeDateStamp);

        CWin32ApplicationView_vtable = base + build.win32ApplicationViewVtable;
        CWin32ApplicationView_v_GetNativeWindow =
            (GetNativeWindow_t)(base + build.win32ApplicationViewGetNativeWindow);
        CWinRTApplicationView_vtable = base + build.winRTApplicationViewVtable;
        CWinRTApplicationView_v_GetNativeWindow =
            (GetNativeWindow_t)(base + build.winRTApplicationViewGetNativeWindow);

        Wh_SetFunctionHook(base + build.isViewVisible,
                           (void*)CVirtualDesktop_IsViewVisible_Hook,
                           (void**)&CVirtualDesktop_IsViewVisible_Original);
        Wh_SetFunctionHook(base + build.altTabViewHostCreateInstance,
                           (void*)XamlAltTabViewHost_CreateInstance_Hook,
                           (void**)&XamlAltTabViewHost_CreateInstance_Original);
        Wh_SetFunctionHook(base + build.taskViewHostCreateInstance,
                           (void*)XamlTaskViewHost_CreateInstance_Hook,
                           (void**)&XamlTaskViewHost_CreateInstance_Original);
        Wh_SetFunctionHook(
            base + build.viewShown,
            (void*)MultitaskingViewVisibilityService_ViewShown_Hook,
            (void**)&MultitaskingViewVisibilityService_ViewShown_Original);
        Wh_SetFunctionHook(
            base + build.viewDismissed,
            (void*)MultitaskingViewVisibilityService_ViewDismissed_Hook,
            (void**)&MultitaskingViewVisibilityService_ViewDismissed_Original);
        return true;
    }

    return false;
}

BOOL Wh_ModInit() {
    HMODULE twinuiPcshellModule = LoadLibrary(L"twinui.pcshell.dll");
    if (!twinuiPcshellModule) {
        Wh_Log(L"Couldn't load twinui.pcshell.dll");
        return FALSE;
    }

    // twinui.pcshell.dll
    WindhawkUtils::SYMBOL_HOOK twinuiPcshellSymbolHooks[] = {
        {
            {LR"(public: virtual long __cdecl CVirtualDesktop::IsViewVisible(struct IApplicationView *,int *))"},
            &CVirtualDesktop_IsViewVisible_Original,
            CVirtualDesktop_IsViewVisible_Hook,
        },
        {
            {LR"(const CWin32ApplicationView::`vftable'{for `IApplicationView'})"},
            &CWin32ApplicationView_vtable,
        },
        {
            {LR"(private: virtual long __cdecl CWin32ApplicationView::v_GetNativeWindow(struct HWND__ * *))"},
            &CWin32ApplicationView_v_GetNativeWindow,
        },
        {
            {LR"(const CWinRTApplicationView::`vftable'{for `IApplicationView'})"},
            &CWinRTApplicationView_vtable,
        },
        {
            {LR"(private: virtual long __cdecl CWinRTApplicationView::v_GetNativeWindow(struct HWND__ * *))"},
            &CWinRTApplicationView_v_GetNativeWindow,
        },
        {
            {LR"(long __cdecl XamlAltTabViewHost_CreateInstance(struct XamlViewHostInitializeArgs const &,struct _GUID const &,void * *))"},
            &XamlAltTabViewHost_CreateInstance_Original,
            XamlAltTabViewHost_CreateInstance_Hook,
        },
        {
            {LR"(long __cdecl XamlTaskViewHost_CreateInstance(struct XamlViewHostInitializeArgs const &,struct _GUID const &,void * *))"},
            &XamlTaskViewHost_CreateInstance_Original,
            XamlTaskViewHost_CreateInstance_Hook,
            true,
        },
        {
            {LR"(public: virtual void __cdecl MultitaskingViewVisibilityService::ViewShown(enum MULTITASKING_VIEW_TYPES))"},
            &MultitaskingViewVisibilityService_ViewShown_Original,
            MultitaskingViewVisibilityService_ViewShown_Hook,
            true,
        },
        {
            {LR"(public: virtual void __cdecl MultitaskingViewVisibilityService::ViewDismissed(enum MULTITASKING_VIEW_TYPES))"},
            &MultitaskingViewVisibilityService_ViewDismissed_Original,
            MultitaskingViewVisibilityService_ViewDismissed_Hook,
            true,
        },
    };

    if (!WindhawkUtils::HookSymbols(twinuiPcshellModule,
                                    twinuiPcshellSymbolHooks,
                                    ARRAYSIZE(twinuiPcshellSymbolHooks))) {
        Wh_Log(L"HookSymbols failed");
        if (!HookKnownBuild(twinuiPcshellModule)) {
            return FALSE;
        }
    }

    return TRUE;
}
