<#
.SYNOPSIS
    Installiert den ESP-Anwesenheit Windows-Client als "Bei Anmeldung"-Task.

.DESCRIPTION
    Kopiert AnwesenheitAgent.ps1 + run-hidden.vbs nach $InstallPath, legt dort
    die Konfigurationsdatei anwesenheit-client.json an und registriert einen
    Scheduled Task ("Bei Anmeldung", fuer JEDEN Benutzer dieses PCs), der den
    Agenten in der jeweiligen Benutzersitzung startet.

    Der Task startet NICHT direkt powershell.exe, sondern run-hidden.vbs ueber
    wscript.exe - so entsteht kein leeres Konsolenfenster (siehe run-hidden.vbs).

    Ohne -ServerUrl arbeitet der Agent im Auto-Discovery-Modus: er findet den
    ESP per UDP-Broadcast selbst (empfohlen). Mit -ServerUrl wird eine feste
    Adresse hinterlegt.

    Muss als Administrator ausgefuehrt werden (Scheduled-Task-Registrierung
    fuer alle Benutzer erfordert das).

.PARAMETER ServerUrl
    OPTIONAL. Adresse des ESP-Anwesenheit-Endpunkts, z.B.
    http://192.168.1.50/event oder http://esp-anwesenheit.local/event.
    Weggelassen => Auto-Discovery (der Agent sucht den ESP per UDP-Broadcast).

.PARAMETER InstallPath
    Zielverzeichnis auf dem lokalen PC. Default: Programme\ESP-Anwesenheit.

.EXAMPLE
    .\Install-AnwesenheitAgent.ps1
    (Auto-Discovery - keine IP noetig)

.EXAMPLE
    .\Install-AnwesenheitAgent.ps1 -ServerUrl "http://192.168.1.50/event"
#>
#Requires -RunAsAdministrator
param(
    [string]$ServerUrl = "",

    [string]$InstallPath = (Join-Path $env:ProgramFiles "ESP-Anwesenheit")
)

$ErrorActionPreference = "Stop"

$sourceScript = Join-Path $PSScriptRoot "AnwesenheitAgent.ps1"
if (-not (Test-Path $sourceScript)) {
    throw "AnwesenheitAgent.ps1 nicht im selben Verzeichnis gefunden: $sourceScript"
}
$sourceVbs = Join-Path $PSScriptRoot "run-hidden.vbs"
if (-not (Test-Path $sourceVbs)) {
    throw "run-hidden.vbs nicht im selben Verzeichnis gefunden: $sourceVbs"
}

New-Item -ItemType Directory -Path $InstallPath -Force | Out-Null
Copy-Item -Path $sourceScript -Destination $InstallPath -Force
Copy-Item -Path $sourceVbs -Destination $InstallPath -Force

$configPath = Join-Path $InstallPath "anwesenheit-client.json"
if ([string]::IsNullOrWhiteSpace($ServerUrl)) {
    # Auto-Discovery-Modus: keine feste URL -> der Agent sucht den ESP per
    # UDP-Broadcast selbst (siehe AnwesenheitAgent.ps1: Invoke-EspDiscovery).
    @{} | ConvertTo-Json | Set-Content -Path $configPath -Encoding UTF8
} else {
    @{ serverUrl = $ServerUrl } | ConvertTo-Json | Set-Content -Path $configPath -Encoding UTF8
}

$taskName = "ESP-Anwesenheit Login Monitor"
$targetScript = Join-Path $InstallPath "AnwesenheitAgent.ps1"
$targetVbs = Join-Path $InstallPath "run-hidden.vbs"

# Start ueber wscript.exe + run-hidden.vbs statt direkt powershell.exe: wscript
# ist eine GUI-Subsystem-Anwendung und erzeugt KEIN Konsolenfenster (das
# fruehere "powershell.exe -WindowStyle Hidden" zeigte in der interaktiven
# Sitzung trotzdem ein leeres Fenster). Das VBS startet den Agenten unsichtbar
# und wartet auf dessen Ende -> der Task bleibt "wird ausgefuehrt" und die
# RestartCount-Selbstheilung sowie das saubere Beenden beim Abmelden greifen.
# Siehe run-hidden.vbs.
$action = New-ScheduledTaskAction -Execute "wscript.exe" -Argument "`"$targetVbs`""
# Kein -User-Parameter -> Trigger feuert fuer JEDE Anmeldung auf diesem PC,
# nicht nur fuer den Benutzer, der gerade installiert.
$trigger = New-ScheduledTaskTrigger -AtLogOn
$settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
    -ExecutionTimeLimit ([TimeSpan]::Zero) -RestartCount 3 -RestartInterval (New-TimeSpan -Minutes 1) `
    -MultipleInstances IgnoreNew
# GroupId ueber die wohlbekannte SID (S-1-5-32-545 = BUILTIN\Users) statt
# des literalen Namens "BUILTIN\Users" - auf nicht-englischen Windows-
# Installationen (z.B. deutsch: "VORDEFINIERT\Benutzer") kann Register-
# ScheduledTask den englischen Namen nicht aufloesen ("Zuordnungen von
# Kontennamen und Sicherheitskennungen wurden nicht durchgefuehrt",
# HRESULT 0x80070534 - real so aufgetreten). Die SID selbst ist
# sprachunabhaengig und funktioniert auf jeder Windows-Lokalisierung.
#
# RunLevel Limited: Standardmuster fuer "bei Anmeldung fuer alle Benutzer,
# jeweils in deren eigener Sitzung" - jeder Benutzer bekommt eine eigene
# Instanz mit eigenen Rechten, kein Admin-Kontext noetig (der Agent
# liest/schreibt nur sein eigenes Log + sendet HTTP).
$principal = New-ScheduledTaskPrincipal -GroupId "S-1-5-32-545" -RunLevel Limited

$modeText = if ([string]::IsNullOrWhiteSpace($ServerUrl)) { "Auto-Discovery (UDP-Broadcast)" } else { $ServerUrl }

Register-ScheduledTask -TaskName $taskName -Action $action -Trigger $trigger `
    -Settings $settings -Principal $principal `
    -Description "Sendet Windows-Anmeldeereignisse (Login/Logout/Lock/RDP) an den ESP-Anwesenheit-Monitor - $modeText." `
    -Force | Out-Null

Write-Host "Installiert: $targetScript (+ run-hidden.vbs, Start ohne Konsolenfenster)"
Write-Host "Scheduled Task '$taskName' registriert (Trigger: Bei Anmeldung, alle Benutzer)."
Write-Host "Server: $modeText (aenderbar in $configPath, wirkt ab dem naechsten Agent-Start)."
Write-Host ""
Write-Host "Der Task startet automatisch bei der naechsten Anmeldung. Zum sofortigen Test in dieser"
Write-Host "Sitzung: Start-ScheduledTask -TaskName '$taskName'"
