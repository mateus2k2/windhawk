# Automatic experiment for the "tab bar stays transparent after full screen" bug.
# Whenever a Groupy tab bar reappears (e.g. on leaving full screen) it waits a moment and applies ONE nudge,
# cycling through the list below. Leave full screen several times, wait a few seconds each time without
# touching anything, and note which attempts made the bar paint by itself.
param(
    [double]$Minutes = 30,
    [int]$DelayMs = 1000,
    # Restrict the experiment to these nudge numbers (1-based); all of them when empty.
    [int[]]$Only,
    # Beep N times just before applying nudge number N, so it is clear which one was used.
    [switch]$Beep,
    [string]$LogFile = (Join-Path $PSScriptRoot "..\captures\tabbar-nudge.log")
)

$src = @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;

public static class TabBarNudge {
    delegate bool EnumWindowsProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] static extern bool EnumWindows(EnumWindowsProc cb, IntPtr l);
    [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr GetPropW(IntPtr h, string name);
    [DllImport("user32.dll", SetLastError = true)] public static extern bool RedrawWindow(IntPtr h, IntPtr rect, IntPtr rgn, uint flags);
    [DllImport("user32.dll", SetLastError = true)] public static extern bool PostMessageW(IntPtr h, uint msg, IntPtr w, IntPtr l);
    [DllImport("user32.dll", SetLastError = true)] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
    [DllImport("user32.dll")] public static extern void NotifyWinEvent(uint ev, IntPtr h, int idObject, int idChild);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindowW(string cls, string title);
    [DllImport("user32.dll")] static extern bool GetCursorPos(out POINT p);
    [DllImport("user32.dll")] static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] static extern bool GetWindowRect(IntPtr h, out RECT r);

    [StructLayout(LayoutKind.Sequential)] struct POINT { public int X, Y; }
    [StructLayout(LayoutKind.Sequential)] struct RECT { public int L, T, R, B; }

    // Moves the real cursor onto the window for a moment, then puts it back.
    public static bool CursorHop(IntPtr h, int holdMs) {
        POINT old; RECT r;
        if (!GetCursorPos(out old) || !GetWindowRect(h, out r)) return false;
        if (!SetCursorPos((r.L + r.R) / 2, (r.T + r.B) / 2)) return false;
        System.Threading.Thread.Sleep(holdMs);
        return SetCursorPos(old.X, old.Y);
    }

    // Groupy's tab bars are tagged with the GP_TAB property.
    public static List<IntPtr> VisibleTabBars() {
        var list = new List<IntPtr>();
        EnumWindows(delegate(IntPtr h, IntPtr l) {
            if (IsWindowVisible(h) && GetPropW(h, "GP_TAB") != IntPtr.Zero) list.Add(h);
            return true;
        }, IntPtr.Zero);
        return list;
    }
}
'@

if (-not ('TabBarNudge' -as [type])) { Add-Type -TypeDefinition $src }

# Round 1 (messages sent straight to the bar, and WM_NCACTIVATE to the browser) did not work:
# Groupy runs elevated and rejects messages to its windows, and the WM_NCACTIVATE pair had no effect.
# Round 2 uses things an unelevated process is allowed to do.
$nudges = @(
    @{ Name = 'fake "title changed" event for the browser window'
       Run  = { param($bar, $live)
                if ($live -eq [IntPtr]::Zero) { return $false }
                # EVENT_OBJECT_NAMECHANGE
                [TabBarNudge]::NotifyWinEvent(0x800C, $live, 0, 0); $true } },
    @{ Name = 'fake "became foreground" event for the browser window'
       Run  = { param($bar, $live)
                if ($live -eq [IntPtr]::Zero) { return $false }
                # EVENT_SYSTEM_FOREGROUND
                [TabBarNudge]::NotifyWinEvent(0x0003, $live, 0, 0); $true } },
    @{ Name = 'fake foreground events: taskbar, then back to the browser'
       Run  = { param($bar, $live)
                $tray = [TabBarNudge]::FindWindowW('Shell_TrayWnd', $null)
                if ($live -eq [IntPtr]::Zero -or $tray -eq [IntPtr]::Zero) { return $false }
                [TabBarNudge]::NotifyWinEvent(0x0003, $tray, 0, 0)
                Start-Sleep -Milliseconds 150
                [TabBarNudge]::NotifyWinEvent(0x0003, $live, 0, 0); $true } },
    @{ Name = 'fake WM_ACTIVATE off + on to the browser window'
       Run  = { param($bar, $live)
                if ($live -eq [IntPtr]::Zero) { return $false }
                [TabBarNudge]::PostMessageW($live, 0x0006, [IntPtr]::Zero, [IntPtr]::Zero) -and
                [TabBarNudge]::PostMessageW($live, 0x0006, [IntPtr]1, [IntPtr]::Zero) } },
    @{ Name = 'real cursor hop onto the bar and back'
       Run  = { param($bar, $live) [TabBarNudge]::CursorHop($bar, 150) } }
)

function Write-Entry([string]$text) {
    $line = "{0} {1}" -f (Get-Date -Format 'HH:mm:ss.fff'), $text
    Add-Content -Path $LogFile -Value $line -Encoding utf8
    $line
}

$numbers = if ($Only) { @($Only | Where-Object { $_ -ge 1 -and $_ -le $nudges.Count }) } else { @(1..$nudges.Count) }
if (-not $numbers.Count) { "No valid nudge numbers in -Only (valid: 1..$($nudges.Count))."; return }

Write-Entry "started; nudges $($numbers -join ',') applied $DelayMs ms after a tab bar reappears"

$known = New-Object 'System.Collections.Generic.HashSet[IntPtr]'
foreach ($b in [TabBarNudge]::VisibleTabBars()) { [void]$known.Add($b) }

$attempt = 0
$deadline = (Get-Date).AddMinutes($Minutes)

while ((Get-Date) -lt $deadline) {
    $now = [TabBarNudge]::VisibleTabBars()
    foreach ($bar in $now) {
        if ($known.Contains($bar)) { continue }

        $number = $numbers[$attempt % $numbers.Count]
        $nudge = $nudges[$number - 1]
        $attempt++
        Start-Sleep -Milliseconds $DelayMs

        if ($Beep) {
            for ($i = 0; $i -lt $number; $i++) { [Console]::Beep(1000, 120); Start-Sleep -Milliseconds 120 }
            Start-Sleep -Milliseconds 300
        }

        $live = [TabBarNudge]::GetPropW($bar, 'GP_WNDLIVE')
        $ok = & $nudge.Run $bar $live
        $err = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
        $result = if ($ok) { 'sent' } else { "FAILED (error $err)" }
        Write-Entry ("attempt #{0}: nudge {1}, {2} -> {3}  (bar 0x{4:X})" -f $attempt, $number, $nudge.Name, $result, $bar.ToInt64())
    }

    $known.Clear()
    foreach ($b in $now) { [void]$known.Add($b) }
    Start-Sleep -Milliseconds 100
}

Write-Entry "done"
