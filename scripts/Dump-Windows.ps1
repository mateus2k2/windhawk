# Dumps every top-level window with the state that decides Alt-Tab / taskbar eligibility:
# styles, owner, DWM cloak state, virtual desktop, and window properties.
# Read-only: it only queries windows, it never changes them.
param(
    [string]$OutFile,
    [switch]$All,
    # Window titles are left out of the output and the saved file unless this is set.
    [switch]$Titles
)

$src = @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;

public class WinInfo {
    public string Hwnd; public int Pid; public string Class; public string Title;
    public bool Visible; public bool Iconic; public string Style; public string ExStyle;
    public bool ToolWindow; public bool AppWindow; public bool NoActivate;
    public string Owner; public int Cloaked; public string Rect;
    public string Desktop; public int OnCurrentDesktop; public string Props; public int Z;
}

[ComImport, Guid("a5cd92ff-29be-454c-8d04-d82879fb3f1b"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
public interface IVirtualDesktopManager {
    [PreserveSig] int IsWindowOnCurrentVirtualDesktop(IntPtr h, out int on);
    [PreserveSig] int GetWindowDesktopId(IntPtr h, out Guid id);
    [PreserveSig] int MoveWindowToDesktop(IntPtr h, ref Guid id);
}

[ComImport, Guid("aa509086-5ca9-4c25-8f95-589d3c07b48a")]
public class VirtualDesktopManagerClass { }

public static class WinDump {
    delegate bool EnumWindowsProc(IntPtr h, IntPtr l);
    delegate bool PropEnumProcEx(IntPtr h, IntPtr str, IntPtr data, IntPtr l);

    [DllImport("user32.dll")] static extern bool EnumWindows(EnumWindowsProc cb, IntPtr l);
    [DllImport("user32.dll")] static extern int EnumPropsExW(IntPtr h, PropEnumProcEx cb, IntPtr l);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetClassNameW(IntPtr h, StringBuilder sb, int n);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetWindowTextW(IntPtr h, StringBuilder sb, int n);
    [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] static extern bool IsIconic(IntPtr h);
    [DllImport("user32.dll")] static extern IntPtr GetWindowLongPtrW(IntPtr h, int idx);
    [DllImport("user32.dll")] static extern IntPtr GetWindow(IntPtr h, uint cmd);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("dwmapi.dll")] static extern int DwmGetWindowAttribute(IntPtr h, int attr, out int val, int size);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)] static extern uint GlobalGetAtomNameW(ushort atom, StringBuilder sb, int n);

    [StructLayout(LayoutKind.Sequential)] struct RECT { public int L, T, R, B; }

    public static List<WinInfo> Run() {
        var list = new List<WinInfo>();
        IVirtualDesktopManager vdm = null;
        try { vdm = (IVirtualDesktopManager)new VirtualDesktopManagerClass(); } catch { }
        int z = 0;
        EnumWindows(delegate(IntPtr h, IntPtr l) {
            var w = new WinInfo();
            w.Z = z++;
            w.Hwnd = "0x" + h.ToInt64().ToString("X");
            uint pid; GetWindowThreadProcessId(h, out pid); w.Pid = (int)pid;
            var sb = new StringBuilder(512);
            GetClassNameW(h, sb, 512); w.Class = sb.ToString();
            sb.Length = 0; GetWindowTextW(h, sb, 512); w.Title = sb.ToString();
            w.Visible = IsWindowVisible(h);
            w.Iconic = IsIconic(h);
            long style = GetWindowLongPtrW(h, -16).ToInt64() & 0xFFFFFFFFL;
            long ex = GetWindowLongPtrW(h, -20).ToInt64() & 0xFFFFFFFFL;
            w.Style = "0x" + style.ToString("X8");
            w.ExStyle = "0x" + ex.ToString("X8");
            w.ToolWindow = (ex & 0x80) != 0;
            w.AppWindow = (ex & 0x40000) != 0;
            w.NoActivate = (ex & 0x08000000) != 0;
            IntPtr owner = GetWindow(h, 4);
            w.Owner = owner == IntPtr.Zero ? "" : "0x" + owner.ToInt64().ToString("X");
            int cloaked = 0; DwmGetWindowAttribute(h, 14, out cloaked, 4); w.Cloaked = cloaked;
            RECT r; GetWindowRect(h, out r);
            w.Rect = r.L + "," + r.T + " " + (r.R - r.L) + "x" + (r.B - r.T);
            w.OnCurrentDesktop = -1; w.Desktop = "";
            if (vdm != null) {
                int on; if (vdm.IsWindowOnCurrentVirtualDesktop(h, out on) == 0) w.OnCurrentDesktop = on;
                Guid g; if (vdm.GetWindowDesktopId(h, out g) == 0) w.Desktop = g.ToString();
            }
            var props = new List<string>();
            EnumPropsExW(h, delegate(IntPtr hh, IntPtr str, IntPtr data, IntPtr ll) {
                string name;
                long v = str.ToInt64();
                if (v < 0x10000) {
                    var a = new StringBuilder(256);
                    name = GlobalGetAtomNameW((ushort)v, a, 256) > 0 ? a.ToString() : "#" + v.ToString("X");
                } else {
                    name = Marshal.PtrToStringUni(str);
                }
                props.Add(name + "=0x" + data.ToInt64().ToString("X"));
                return true;
            }, IntPtr.Zero);
            w.Props = string.Join("; ", props.ToArray());
            list.Add(w);
            return true;
        }, IntPtr.Zero);
        return list;
    }
}
'@

if (-not ('WinDump' -as [type])) { Add-Type -TypeDefinition $src }

$procs = @{}
Get-Process | ForEach-Object { $procs[$_.Id] = $_.Name }

$wins = [WinDump]::Run() | ForEach-Object {
    if (-not $Titles -and $_.Title) { $_.Title = "<$($_.Title.Length) chars>" }
    $_ | Add-Member -NotePropertyName Process -NotePropertyValue $procs[$_.Pid] -PassThru
}

if ($OutFile) {
    $dir = Split-Path $OutFile -Parent
    if ($dir -and -not (Test-Path $dir)) { New-Item -ItemType Directory -Force $dir | Out-Null }
    $wins | ConvertTo-Json -Depth 3 | Out-File $OutFile -Encoding utf8
}

# By default show only windows that matter here: visible ones with a title, and anything Groupy owns or tagged.
$shown = if ($All) { $wins } else {
    $wins | Where-Object { ($_.Visible -and $_.Title) -or $_.Process -match 'Groupy' -or $_.Props -match 'Groupy|Stardock|WDG|SD_' -or $_.Class -match 'Groupy|Stardock' }
}

foreach ($w in $shown) {
    $flags = @()
    if ($w.Visible) { $flags += 'VIS' } else { $flags += 'hidden' }
    if ($w.Iconic) { $flags += 'MIN' }
    if ($w.ToolWindow) { $flags += 'TOOL' }
    if ($w.AppWindow) { $flags += 'APPWIN' }
    if ($w.NoActivate) { $flags += 'NOACT' }
    if ($w.Cloaked) { $flags += "CLOAK$($w.Cloaked)" }
    if ($w.Owner) { $flags += "owner=$($w.Owner)" }
    $title = if ($w.Title.Length -gt 50) { $w.Title.Substring(0, 50) + '...' } else { $w.Title }
    "{0,-10} z{1,-4} {2,-18} [{3}] '{4}'" -f $w.Hwnd, $w.Z, $w.Process, $w.Class, $title
    "             {0}  style={1} ex={2}  rect={3}  desk={4} cur={5}" -f ($flags -join ' '), $w.Style, $w.ExStyle, $w.Rect, $w.Desktop, $w.OnCurrentDesktop
    if ($w.Props) { "             props: $($w.Props)" }
}
"`n$($shown.Count) shown of $($wins.Count) top-level windows"
