# Logs every change in size, style, placement and Groupy properties of one program's windows.
# Used to capture what happens around a bug (e.g. leaving full screen in a grouped Firefox window).
# Read-only: it only queries windows. Window titles are never recorded.
param(
    [string[]]$Process = 'firefox',
    [string]$LogFile = (Join-Path $PSScriptRoot "..\captures\watch-$($Process -join '+')-$(Get-Date -Format yyyyMMdd-HHmmss).log"),
    [double]$Minutes = 60,
    [int]$IntervalMs = 100
)

$src = @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;

public static class WinWatch {
    delegate bool EnumWindowsProc(IntPtr h, IntPtr l);
    delegate bool PropEnumProcEx(IntPtr h, IntPtr str, IntPtr data, IntPtr l);

    [DllImport("user32.dll")] static extern bool EnumWindows(EnumWindowsProc cb, IntPtr l);
    [DllImport("user32.dll")] static extern int EnumPropsExW(IntPtr h, PropEnumProcEx cb, IntPtr l);
    [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] static extern IntPtr GetWindowLongPtrW(IntPtr h, int idx);
    [DllImport("user32.dll")] static extern IntPtr GetWindow(IntPtr h, uint cmd);
    [DllImport("user32.dll")] static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] static extern bool GetWindowPlacement(IntPtr h, ref WINDOWPLACEMENT p);
    [DllImport("dwmapi.dll")] static extern int DwmGetWindowAttribute(IntPtr h, int attr, out int val, int size);
    [DllImport("user32.dll")] static extern bool GetLayeredWindowAttributes(IntPtr h, out uint key, out byte alpha, out uint flags);

    [StructLayout(LayoutKind.Sequential)] struct RECT { public int L, T, R, B; }
    [StructLayout(LayoutKind.Sequential)] struct POINT { public int X, Y; }
    [StructLayout(LayoutKind.Sequential)] struct WINDOWPLACEMENT {
        public int length, flags, showCmd; public POINT min, max; public RECT normal;
    }

    static string R(RECT r) { return r.L + "," + r.T + " " + (r.R - r.L) + "x" + (r.B - r.T); }

    // One entry per visible, unowned top-level window of the given processes: hwnd -> state string.
    public static Dictionary<string, string> Snapshot(int[] pids) {
        var set = new HashSet<int>(pids);
        var result = new Dictionary<string, string>();
        IntPtr fg = GetForegroundWindow();
        EnumWindows(delegate(IntPtr h, IntPtr l) {
            uint pid; GetWindowThreadProcessId(h, out pid);
            if (!set.Contains((int)pid) || !IsWindowVisible(h) || GetWindow(h, 4) != IntPtr.Zero) return true;
            long style = GetWindowLongPtrW(h, -16).ToInt64() & 0xFFFFFFFFL;
            long ex = GetWindowLongPtrW(h, -20).ToInt64() & 0xFFFFFFFFL;
            RECT r; GetWindowRect(h, out r);
            var p = new WINDOWPLACEMENT(); p.length = Marshal.SizeOf(typeof(WINDOWPLACEMENT));
            GetWindowPlacement(h, ref p);
            int cloak = 0; DwmGetWindowAttribute(h, 14, out cloak, 4);
            var props = new List<string>();
            EnumPropsExW(h, delegate(IntPtr hh, IntPtr str, IntPtr data, IntPtr ll) {
                if (str.ToInt64() >= 0x10000) {
                    string name = Marshal.PtrToStringUni(str);
                    // GP_LACTTIME changes on every activation and would drown out real changes.
                    if (name.StartsWith("GP_") && name != "GP_LACTTIME") props.Add(name + "=" + data.ToInt64().ToString("X"));
                }
                return true;
            }, IntPtr.Zero);
            props.Sort();
            var sb = new StringBuilder();
            sb.Append("style=").Append(style.ToString("X8"));
            sb.Append((style & 0x01000000) != 0 ? "(MAX" : "(");
            sb.Append((style & 0x20000000) != 0 ? " MIN" : "");
            sb.Append((style & 0x00C00000) == 0x00C00000 ? " CAPTION)" : " nocaption)");
            sb.Append(" ex=").Append(ex.ToString("X8"));
            sb.Append(" rect=").Append(R(r));
            sb.Append(" showCmd=").Append(p.showCmd).Append(" plFlags=").Append(p.flags);
            sb.Append(" normal=").Append(R(p.normal));
            sb.Append(" cloak=").Append(cloak);
            if ((ex & 0x80000) != 0) {
                // Fails for windows drawn with UpdateLayeredWindow, which have no queryable alpha.
                uint key; byte alpha; uint lflags;
                if (GetLayeredWindowAttributes(h, out key, out alpha, out lflags)) sb.Append(" layered=alpha" + alpha + "/flags" + lflags);
                else sb.Append(" layered=ULW");
            }
            sb.Append(h == fg ? " FOREGROUND" : "");
            sb.Append(" | ").Append(string.Join(" ", props.ToArray()));
            result["0x" + h.ToInt64().ToString("X")] = sb.ToString();
            return true;
        }, IntPtr.Zero);
        return result;
    }
}
'@

if (-not ('WinWatch' -as [type])) { Add-Type -TypeDefinition $src }

$dir = Split-Path $LogFile -Parent
if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Force $dir | Out-Null }

function Write-Entry([string]$text) {
    $line = "{0} {1}" -f (Get-Date -Format 'HH:mm:ss.fff'), $text
    Add-Content -Path $LogFile -Value $line -Encoding utf8
    $line
}

Write-Entry "watching '$Process' for $Minutes min, every $IntervalMs ms -> $LogFile"

$last = @{}
$deadline = (Get-Date).AddMinutes($Minutes)
$pids = @()
$nextPidRefresh = Get-Date

while ((Get-Date) -lt $deadline) {
    if ((Get-Date) -ge $nextPidRefresh) {
        $pids = @(Get-Process -Name $Process -ErrorAction SilentlyContinue | ForEach-Object { $_.Id })
        $nextPidRefresh = (Get-Date).AddSeconds(5)
    }

    $now = [WinWatch]::Snapshot([int[]]$pids)

    foreach ($h in $now.Keys) {
        if (-not $last.ContainsKey($h)) { Write-Entry "$h NEW     $($now[$h])" }
        elseif ($last[$h] -ne $now[$h]) { Write-Entry "$h CHANGED $($now[$h])" }
    }
    foreach ($h in @($last.Keys)) {
        if (-not $now.ContainsKey($h)) { Write-Entry "$h GONE" }
    }

    $last = $now
    Start-Sleep -Milliseconds $IntervalMs
}

Write-Entry "done"
