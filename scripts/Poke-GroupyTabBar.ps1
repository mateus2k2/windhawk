# Experiment for the "tab bar stays transparent after full screen" bug.
# Run it while the bar is transparent, WITHOUT moving the mouse over the bar.
# It tries a series of harmless nudges on Groupy's visible tab bar windows, one every few seconds,
# and prints which one it is on, so you can see which nudge (if any) makes the bar reappear.
param(
    [int]$PauseSeconds = 4
)

$src = @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;

public static class TabBarPoke {
    delegate bool EnumWindowsProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] static extern bool EnumWindows(EnumWindowsProc cb, IntPtr l);
    [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern IntPtr GetPropW(IntPtr h, string name);
    [DllImport("user32.dll")] static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll", SetLastError = true)] public static extern bool RedrawWindow(IntPtr h, IntPtr rect, IntPtr rgn, uint flags);
    [DllImport("user32.dll", SetLastError = true)] public static extern bool PostMessageW(IntPtr h, uint msg, IntPtr w, IntPtr l);
    [DllImport("user32.dll", SetLastError = true)] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);

    [StructLayout(LayoutKind.Sequential)] struct RECT { public int L, T, R, B; }

    // Groupy's tab bars are tagged with the GP_TAB property.
    public static List<IntPtr> VisibleTabBars() {
        var list = new List<IntPtr>();
        EnumWindows(delegate(IntPtr h, IntPtr l) {
            if (IsWindowVisible(h) && GetPropW(h, "GP_TAB") != IntPtr.Zero) list.Add(h);
            return true;
        }, IntPtr.Zero);
        return list;
    }

    public static string Describe(IntPtr h) {
        RECT r; GetWindowRect(h, out r);
        return "0x" + h.ToInt64().ToString("X") + " at " + r.L + "," + r.T + " " + (r.R - r.L) + "x" + (r.B - r.T);
    }
}
'@

if (-not ('TabBarPoke' -as [type])) { Add-Type -TypeDefinition $src }

$bars = [TabBarPoke]::VisibleTabBars()
if (-not $bars.Count) { "No visible Groupy tab bar found."; return }
"Tab bars: " + (($bars | ForEach-Object { [TabBarPoke]::Describe($_) }) -join ' | ')

function Invoke-Step([string]$name, [scriptblock]$action) {
    "`nSTEP $name"
    foreach ($b in $bars) {
        $ok = & $action $b
        $err = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
        "  $([TabBarPoke]::Describe($b)): " + $(if ($ok) { 'sent' } else { "FAILED (error $err; 5 means Groupy runs elevated and blocks this)" })
    }
    "  ...look at the bar now ($PauseSeconds s)"
    Start-Sleep -Seconds $PauseSeconds
}

# RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW | RDW_FRAME
Invoke-Step '1: redraw request'            { param($h) [TabBarPoke]::RedrawWindow($h, [IntPtr]::Zero, [IntPtr]::Zero, 0x0585) }
# SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED
Invoke-Step '2: frame-changed notification' { param($h) [TabBarPoke]::SetWindowPos($h, [IntPtr]::Zero, 0, 0, 0, 0, 0x0037) }
# WM_MOUSEMOVE at client point (200, 10), as if the mouse had entered the bar
Invoke-Step '3: fake mouse move'           { param($h) [TabBarPoke]::PostMessageW($h, 0x0200, [IntPtr]::Zero, [IntPtr]((10 -shl 16) -bor 200)) }
# WM_MOUSELEAVE, to undo any hover state from step 3
Invoke-Step '4: fake mouse leave'          { param($h) [TabBarPoke]::PostMessageW($h, 0x02A3, [IntPtr]::Zero, [IntPtr]::Zero) }

"`nDone. Tell Claude which step, if any, made the bar reappear."
