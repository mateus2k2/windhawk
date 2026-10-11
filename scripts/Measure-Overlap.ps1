# Measures how far Groupy's tab bar overlaps the content of each group's active window.
# Read-only. Windows on other virtual desktops are included; their geometry is still valid.
param(
    # Include the inactive tabs of each group, not just the active one.
    [switch]$All
)

$src = @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;

public static class OverlapProbe {
    delegate bool EnumWindowsProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] static extern bool EnumWindows(EnumWindowsProc cb, IntPtr l);
    [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] static extern bool IsZoomed(IntPtr h);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern IntPtr GetPropW(IntPtr h, string name);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetClassNameW(IntPtr h, StringBuilder sb, int n);
    [DllImport("user32.dll")] static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] static extern bool ClientToScreen(IntPtr h, ref POINT p);
    [DllImport("user32.dll")] static extern uint GetDpiForWindow(IntPtr h);
    [DllImport("dwmapi.dll")] static extern int DwmGetWindowAttribute(IntPtr h, int attr, out int val, int size);

    [StructLayout(LayoutKind.Sequential)] struct RECT { public int L, T, R, B; }
    [StructLayout(LayoutKind.Sequential)] struct POINT { public int X, Y; }

    static string S(RECT r) { return r.L + "," + r.T + " " + (r.R - r.L) + "x" + (r.B - r.T); }

    public static List<string> Run(bool all) {
        var o = new List<string>();
        EnumWindows(delegate(IntPtr h, IntPtr l) {
            IntPtr bar = GetPropW(h, "GP_LINK");
            if (bar == IntPtr.Zero || !IsWindowVisible(h)) return true;
            bool live = GetPropW(h, "GP_LIVE") != IntPtr.Zero;
            if (!live && !all) return true;
            int cloak = 0; DwmGetWindowAttribute(h, 14, out cloak, 4);
            var sb = new StringBuilder(128); GetClassNameW(h, sb, 128);
            RECT w, c, b; GetWindowRect(h, out w); GetClientRect(h, out c); GetWindowRect(bar, out b);
            POINT p = new POINT(); ClientToScreen(h, ref p);
            o.Add(sb + " 0x" + h.ToInt64().ToString("X") + " maximized=" + IsZoomed(h) + " active=" + live + " cloak=" + cloak + " dpi=" + GetDpiForWindow(h));
            o.Add("   window " + S(w) + "   client starts at " + p.X + "," + p.Y + " size " + c.R + "x" + c.B);
            o.Add("   tab bar " + S(b) + " shown=" + IsWindowVisible(bar));
            o.Add("   bar bottom " + b.B + ", client top " + p.Y + " => overlap " + (b.B - p.Y) + " px");
            return true;
        }, IntPtr.Zero);
        return o;
    }
}
'@

if (-not ('OverlapProbe' -as [type])) { Add-Type -TypeDefinition $src }

$lines = [OverlapProbe]::Run([bool]$All)
if ($lines.Count) { $lines } else { "No grouped windows found." }
