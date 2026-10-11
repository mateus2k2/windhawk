// ==WindhawkMod==
// @id              groupy-bar-gap-fix
// @name            Groupy 2: tab bar gap fix
// @description     Fills the unpainted strip Groupy 2 can leave at the right end of a maximized group's tab bar.
// @version         0.1
// @author          mateu
// @include         GroupyCtrl.exe
// @compilerOptions -lgdi32
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Groupy 2: tab bar gap fix

Groupy stops painting its tab bar a few pixels before the right edge of the
screen (seen on a 1366 pixel wide display). This shows on a maximized group,
and on any grouped window pushed against or past the right side. The last few
pixels stay transparent, so whatever is underneath shows through and receives
the clicks.

This mod runs inside `GroupyCtrl.exe` and extends what Groupy paints: when a
bar that reaches the right edge of the screen stops a few columns short of it,
those columns are filled with a copy of the last painted column.

Groupy runs elevated, so Windhawk has to run as administrator (or be installed
as a service) for this mod to load.
*/
// ==/WindhawkModReadme==

#include <windows.h>

#include <atomic>

// Wider transparent endings than this are not the gap this mod is about.
constexpr int kMaxGapToFill = 32;

std::atomic<int> g_lastLoggedGap{-1};

// The right edge of the monitor the window is on, in screen coordinates.
bool GetMonitorRightEdge(HWND hwnd, int* rightEdge) {
    MONITORINFO monitorInfo = {};
    monitorInfo.cbSize = sizeof(monitorInfo);
    HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    if (!monitor || !GetMonitorInfoW(monitor, &monitorInfo)) {
        return false;
    }

    *rightEdge = monitorInfo.rcMonitor.right;
    return true;
}

// A copy of the image Groupy is about to show on a tab bar, with the
// transparent strip at its right end filled in.
struct FilledImage {
    HDC dc = nullptr;
    HBITMAP bitmap = nullptr;
    HGDIOBJ previousBitmap = nullptr;

    ~FilledImage() {
        if (dc) {
            if (previousBitmap) {
                SelectObject(dc, previousBitmap);
            }
            DeleteDC(dc);
        }
        if (bitmap) {
            DeleteObject(bitmap);
        }
    }
};

// Returns false, leaving `image` unusable, whenever the update isn't a tab bar
// with a small transparent strip at its right end. The caller then passes
// Groupy's own image through unchanged.
bool BuildFilledImage(HWND hwnd,
                      const POINT* destination,
                      const SIZE* size,
                      HDC sourceDc,
                      const POINT* sourceOrigin,
                      FilledImage& image) {
    if (!hwnd || !size || !sourceDc || !GetPropW(hwnd, L"GP_TAB")) {
        return false;
    }

    int width = size->cx;
    int height = size->cy;
    if (width <= kMaxGapToFill || height <= 0 || width > 16384 ||
        height > 1024) {
        return false;
    }

    int left;
    if (destination) {
        left = destination->x;
    } else {
        RECT windowRect;
        if (!GetWindowRect(hwnd, &windowRect)) {
            return false;
        }
        left = windowRect.left;
    }

    // Groupy stops painting a few pixels before the right edge of the screen.
    // That only matters for a bar that reaches the edge or runs past it: a
    // maximized group, or a window pushed against or beyond the right side.
    int monitorRight;
    if (!GetMonitorRightEdge(hwnd, &monitorRight) ||
        left + width < monitorRight) {
        return false;
    }

    // The part of the image that is on this monitor.
    int visibleWidth = monitorRight - left;
    if (visibleWidth <= kMaxGapToFill || visibleWidth > width) {
        return false;
    }

    BITMAPINFO bitmapInfo = {};
    bitmapInfo.bmiHeader.biSize = sizeof(bitmapInfo.bmiHeader);
    bitmapInfo.bmiHeader.biWidth = width;
    bitmapInfo.bmiHeader.biHeight = -height;  // top-down
    bitmapInfo.bmiHeader.biPlanes = 1;
    bitmapInfo.bmiHeader.biBitCount = 32;
    bitmapInfo.bmiHeader.biCompression = BI_RGB;

    DWORD* pixels = nullptr;
    image.dc = CreateCompatibleDC(sourceDc);
    if (!image.dc) {
        return false;
    }

    image.bitmap = CreateDIBSection(image.dc, &bitmapInfo, DIB_RGB_COLORS,
                                    (void**)&pixels, nullptr, 0);
    if (!image.bitmap || !pixels) {
        return false;
    }

    image.previousBitmap = SelectObject(image.dc, image.bitmap);
    if (!image.previousBitmap) {
        return false;
    }

    int sourceX = sourceOrigin ? sourceOrigin->x : 0;
    int sourceY = sourceOrigin ? sourceOrigin->y : 0;
    if (!BitBlt(image.dc, 0, 0, width, height, sourceDc, sourceX, sourceY,
                SRCCOPY)) {
        return false;
    }
    GdiFlush();

    // Find where the painting stops short of the screen edge, judging by the
    // middle row.
    const DWORD* middleRow = pixels + (size_t)(height / 2) * width;
    int paintedWidth = visibleWidth;
    while (paintedWidth > visibleWidth - kMaxGapToFill - 1 &&
           (middleRow[paintedWidth - 1] >> 24) == 0) {
        paintedWidth--;
    }

    int gap = visibleWidth - paintedWidth;
    if (gap == 0 || gap > kMaxGapToFill) {
        return false;
    }

    if (g_lastLoggedGap.exchange(gap) != gap) {
        Wh_Log(L"Filling a %d px gap on a %dx%d tab bar", gap, width, height);
    }

    for (int y = 0; y < height; y++) {
        DWORD* row = pixels + (size_t)y * width;
        DWORD fill = row[paintedWidth - 1];
        if ((fill >> 24) == 0) {
            continue;
        }

        for (int x = paintedWidth; x < visibleWidth; x++) {
            // Leave anything Groupy did paint in the strip.
            if ((row[x] >> 24) == 0) {
                row[x] = fill;
            }
        }
    }

    return true;
}

using UpdateLayeredWindow_t = decltype(&UpdateLayeredWindow);
UpdateLayeredWindow_t UpdateLayeredWindow_Original;
BOOL WINAPI UpdateLayeredWindow_Hook(HWND hwnd,
                                     HDC destinationDc,
                                     POINT* destination,
                                     SIZE* size,
                                     HDC sourceDc,
                                     POINT* sourceOrigin,
                                     COLORREF colorKey,
                                     BLENDFUNCTION* blend,
                                     DWORD flags) {
    FilledImage image;
    if (BuildFilledImage(hwnd, destination, size, sourceDc, sourceOrigin,
                         image)) {
        POINT origin = {0, 0};
        return UpdateLayeredWindow_Original(hwnd, destinationDc, destination,
                                            size, image.dc, &origin, colorKey,
                                            blend, flags);
    }

    return UpdateLayeredWindow_Original(hwnd, destinationDc, destination, size,
                                        sourceDc, sourceOrigin, colorKey, blend,
                                        flags);
}

using UpdateLayeredWindowIndirect_t = decltype(&UpdateLayeredWindowIndirect);
UpdateLayeredWindowIndirect_t UpdateLayeredWindowIndirect_Original;
BOOL WINAPI UpdateLayeredWindowIndirect_Hook(HWND hwnd,
                                             const UPDATELAYEREDWINDOWINFO* info) {
    FilledImage image;
    if (info && info->cbSize >= sizeof(UPDATELAYEREDWINDOWINFO) &&
        BuildFilledImage(hwnd, info->pptDst, info->psize, info->hdcSrc,
                         info->pptSrc, image)) {
        POINT origin = {0, 0};
        UPDATELAYEREDWINDOWINFO filledInfo = *info;
        filledInfo.hdcSrc = image.dc;
        filledInfo.pptSrc = &origin;
        // The replacement image is complete, so don't limit the update to the
        // part Groupy considered changed.
        filledInfo.prcDirty = nullptr;
        return UpdateLayeredWindowIndirect_Original(hwnd, &filledInfo);
    }

    return UpdateLayeredWindowIndirect_Original(hwnd, info);
}

BOOL Wh_ModInit() {
    if (!Wh_SetFunctionHook((void*)UpdateLayeredWindow,
                            (void*)UpdateLayeredWindow_Hook,
                            (void**)&UpdateLayeredWindow_Original)) {
        Wh_Log(L"Hooking UpdateLayeredWindow failed");
        return FALSE;
    }

    if (!Wh_SetFunctionHook((void*)UpdateLayeredWindowIndirect,
                            (void*)UpdateLayeredWindowIndirect_Hook,
                            (void**)&UpdateLayeredWindowIndirect_Original)) {
        Wh_Log(L"Hooking UpdateLayeredWindowIndirect failed");
        return FALSE;
    }

    return TRUE;
}
