# Creates a scheduled task that starts the portable Windhawk elevated, in the tray, when you log on.
# Elevation is needed for mods that load into Groupy, which itself runs elevated.
# Run this from an elevated PowerShell (Run as administrator).
param(
    # Full path to the portable windhawk.exe on this computer.
    [Parameter(Mandatory)]
    [string]$WindhawkPath,
    [string]$TaskName = 'Windhawk (elevated at logon)'
)

if (-not (Test-Path $WindhawkPath -PathType Leaf)) { throw "Not found: $WindhawkPath" }
$WindhawkPath = (Resolve-Path $WindhawkPath).Path

$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) { throw 'Run this from an elevated PowerShell (Run as administrator).' }

$user = [Security.Principal.WindowsIdentity]::GetCurrent().Name

$action = New-ScheduledTaskAction -Execute $WindhawkPath -Argument '-tray-only' -WorkingDirectory (Split-Path $WindhawkPath)
$trigger = New-ScheduledTaskTrigger -AtLogOn -User $user
$principal = New-ScheduledTaskPrincipal -UserId $user -LogonType Interactive -RunLevel Highest
# No time limit, and don't skip or stop it on battery: Windhawk has to stay running.
$settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
    -ExecutionTimeLimit ([TimeSpan]::Zero) -MultipleInstances IgnoreNew

Register-ScheduledTask -TaskName $TaskName -Action $action -Trigger $trigger -Principal $principal -Settings $settings -Force | Out-Null

"Created task '$TaskName' for $user"
"It runs: `"$WindhawkPath`" -tray-only (elevated) at logon."
"To start it now without logging off: Start-ScheduledTask -TaskName '$TaskName'"
"To remove it: Unregister-ScheduledTask -TaskName '$TaskName' -Confirm:`$false"
