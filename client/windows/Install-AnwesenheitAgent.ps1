<#
.SYNOPSIS
    Installiert den ESP-Anwesenheit Windows-Client als "Bei Anmeldung"-Task.

.DESCRIPTION
    Kopiert AnwesenheitAgent.ps1 nach $InstallPath, legt dort die
    Konfigurationsdatei anwesenheit-client.json mit der Serveradresse an und
    registriert einen Scheduled Task ("Bei Anmeldung", fuer JEDEN Benutzer
    dieses PCs), der den Agenten in der jeweiligen Benutzersitzung startet.

    Muss als Administrator ausgefuehrt werden (Scheduled-Task-Registrierung
    fuer alle Benutzer erfordert das).

.PARAMETER ServerUrl
    Adresse des ESP-Anwesenheit-Endpunkts, z.B. http://192.168.1.50/event
    oder http://esp-anwesenheit.local/event (mDNS, siehe Firmware).

.PARAMETER InstallPath
    Zielverzeichnis auf dem lokalen PC. Default: Programme\ESP-Anwesenheit.

.EXAMPLE
    .\Install-AnwesenheitAgent.ps1 -ServerUrl "http://192.168.1.50/event"
#>
#Requires -RunAsAdministrator
param(
    [Parameter(Mandatory)]
    [string]$ServerUrl,

    [string]$InstallPath = (Join-Path $env:ProgramFiles "ESP-Anwesenheit")
)

$ErrorActionPreference = "Stop"

$sourceScript = Join-Path $PSScriptRoot "AnwesenheitAgent.ps1"
if (-not (Test-Path $sourceScript)) {
    throw "AnwesenheitAgent.ps1 nicht im selben Verzeichnis gefunden: $sourceScript"
}

New-Item -ItemType Directory -Path $InstallPath -Force | Out-Null
Copy-Item -Path $sourceScript -Destination $InstallPath -Force

$configPath = Join-Path $InstallPath "anwesenheit-client.json"
@{ serverUrl = $ServerUrl } | ConvertTo-Json | Set-Content -Path $configPath -Encoding UTF8

$taskName = "ESP-Anwesenheit Login Monitor"
$targetScript = Join-Path $InstallPath "AnwesenheitAgent.ps1"

$action = New-ScheduledTaskAction -Execute "powershell.exe" `
    -Argument "-NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File `"$targetScript`""
# Kein -User-Parameter -> Trigger feuert fuer JEDE Anmeldung auf diesem PC,
# nicht nur fuer den Benutzer, der gerade installiert.
$trigger = New-ScheduledTaskTrigger -AtLogOn
$settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
    -ExecutionTimeLimit ([TimeSpan]::Zero) -RestartCount 3 -RestartInterval (New-TimeSpan -Minutes 1) `
    -MultipleInstances IgnoreNew
# GroupId "BUILTIN\Users" + RunLevel Limited: Standardmuster fuer "bei
# Anmeldung fuer alle Benutzer, jeweils in deren eigener Sitzung" - jeder
# Benutzer bekommt eine eigene Instanz mit eigenen Rechten, kein Admin-Kontext
# noetig (der Agent liest/schreibt nur sein eigenes Log + sendet HTTP).
$principal = New-ScheduledTaskPrincipal -GroupId "BUILTIN\Users" -RunLevel Limited

Register-ScheduledTask -TaskName $taskName -Action $action -Trigger $trigger `
    -Settings $settings -Principal $principal `
    -Description "Sendet Windows-Anmeldeereignisse (Login/Logout/Lock/RDP) an den ESP-Anwesenheit-Monitor ($ServerUrl)." `
    -Force | Out-Null

Write-Host "Installiert: $targetScript"
Write-Host "Scheduled Task '$taskName' registriert (Trigger: Bei Anmeldung, alle Benutzer)."
Write-Host "Server-URL: $ServerUrl (aenderbar in $configPath, wirkt ab dem naechsten Agent-Start)."
Write-Host ""
Write-Host "Der Task startet automatisch bei der naechsten Anmeldung. Zum sofortigen Test in dieser"
Write-Host "Sitzung: Start-ScheduledTask -TaskName '$taskName'"
