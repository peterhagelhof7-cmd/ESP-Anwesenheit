<#
.SYNOPSIS
    Entfernt den ESP-Anwesenheit Windows-Client (Scheduled Task + Dateien).
#>
#Requires -RunAsAdministrator
param(
    [string]$InstallPath = (Join-Path $env:ProgramFiles "ESP-Anwesenheit")
)

$taskName = "ESP-Anwesenheit Login Monitor"

Get-ScheduledTask -TaskName $taskName -ErrorAction SilentlyContinue | Stop-ScheduledTask -ErrorAction SilentlyContinue
Unregister-ScheduledTask -TaskName $taskName -Confirm:$false -ErrorAction SilentlyContinue

if (Test-Path $InstallPath) {
    Remove-Item -Path $InstallPath -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Host "ESP-Anwesenheit Windows-Client deinstalliert (Task + $InstallPath)."
