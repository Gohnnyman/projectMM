# Remove projectMM, keeping your settings.
#
# The companion to Install-projectMM.ps1, and the target of the Start-menu "Uninstall projectMM"
# entry. It removes the program and nothing else.
#
# Your settings are NOT touched. They live in %LOCALAPPDATA%\projectMM, deliberately separate from
# the program, so a reinstall or an upgrade finds your layouts, effects and drivers exactly where
# you left them. Delete that folder by hand if you want a genuinely clean slate.

$ErrorActionPreference = "Stop"

$InstallTo = Join-Path $env:LOCALAPPDATA "Programs\projectMM"
$StartMenu = Join-Path $env:APPDATA "Microsoft\Windows\Start Menu\Programs\projectMM"
$RegKey    = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\projectMM"
$Settings  = Join-Path $env:LOCALAPPDATA "projectMM"

$running = Get-Process -Name projectMM -ErrorAction SilentlyContinue
if ($running) {
    Write-Host "Stopping the running projectMM..."
    $running | Stop-Process -Force
    Start-Sleep -Milliseconds 800
}

# This script runs FROM the directory it is about to delete, so the shell holds it open on
# Windows. Removing the contents and leaving the directory is the honest outcome: the program is
# gone, and an empty folder disappears on the next reboot or with one manual delete.
if (Test-Path $InstallTo) {
    Write-Host "Removing $InstallTo"
    Get-ChildItem $InstallTo -Force -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -ne "Uninstall-projectMM.ps1" } |
        Remove-Item -Recurse -Force -ErrorAction SilentlyContinue
}

if (Test-Path $StartMenu) {
    Write-Host "Removing the Start-menu entry"
    Remove-Item $StartMenu -Recurse -Force -ErrorAction SilentlyContinue
}

if (Test-Path $RegKey) {
    Write-Host "Removing the Add/Remove Programs entry"
    Remove-Item $RegKey -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Host ""
Write-Host "projectMM removed." -ForegroundColor Green
if (Test-Path $Settings) {
    Write-Host "Your settings are kept in $Settings. Delete that folder to start completely fresh."
}
Write-Host "One file remains, this script itself, because it is running from that folder. Delete $InstallTo when convenient."
