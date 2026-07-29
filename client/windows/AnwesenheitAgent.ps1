<#
.SYNOPSIS
    ESP-Anwesenheit Windows-Client.

.DESCRIPTION
    Ueberwacht Sitzungswechsel (Anmeldung, Abmeldung, Sperren, Entsperren,
    RDP-Verbindung/-Trennung) ueber Microsoft.Win32.SystemEvents.SessionSwitch
    und meldet sie per HTTP POST /event an den ESP-Anwesenheit-Monitor (siehe
    Projektbeschreibung.txt im Projekt-Root).

    Wird ueber einen "Bei Anmeldung"-Scheduled-Task gestartet (siehe
    Install-AnwesenheitAgent.ps1) und laeuft im Kontext des jeweils
    angemeldeten Benutzers, solange die Sitzung besteht.

    Serveradresse kommt aus anwesenheit-client.json im selben Verzeichnis
    (wird von Install-AnwesenheitAgent.ps1 angelegt) - Default, falls die
    Datei fehlt: http://esp-anwesenheit.local/event
#>

Add-Type -AssemblyName System.Windows.Forms

$script:ScriptDir = if ($PSScriptRoot) { $PSScriptRoot } else { Split-Path -Parent $MyInvocation.MyCommand.Path }
$script:ConfigPath = Join-Path $script:ScriptDir "anwesenheit-client.json"
$script:LogPath = Join-Path $script:ScriptDir "client.log"
$script:MaxLogBytes = 1MB

function Get-ServerUrl {
    if (Test-Path $script:ConfigPath) {
        try {
            $cfg = Get-Content $script:ConfigPath -Raw | ConvertFrom-Json
            if ($cfg.serverUrl) { return [string]$cfg.serverUrl }
        } catch {
            # ungueltiges JSON -> auf Default zurueckfallen, kein Absturz
        }
    }
    return "http://esp-anwesenheit.local/event"
}
$script:ServerUrl = Get-ServerUrl

# Einfaches Rolling-Log fuer die Diagnose vor Ort (kein Netzwerkzugriff noetig,
# um zu sehen, ob der Agent ueberhaupt laeuft/sendet) - Datei wird bei
# Ueberschreiten von MaxLogBytes verworfen statt rotiert, da fuer einen
# schlanken Client eine zweite Logdatei unnoetigen Aufwand bedeuten wuerde.
function Write-AgentLog {
    param([string]$Message)
    try {
        if ((Test-Path $script:LogPath) -and (Get-Item $script:LogPath).Length -gt $script:MaxLogBytes) {
            Remove-Item $script:LogPath -Force -ErrorAction SilentlyContinue
        }
        $line = "{0:yyyy-MM-dd HH:mm:ss} {1}" -f (Get-Date), $Message
        Add-Content -Path $script:LogPath -Value $line -Encoding UTF8
    } catch {
        # Logging darf den Agenten nie zum Absturz bringen (z.B. Verzeichnis
        # schreibgeschuetzt) - Fehler hier wird bewusst verschluckt.
    }
}

function Get-CurrentLogonType {
    if ([System.Windows.Forms.SystemInformation]::TerminalServerSession) { return "RDP" }
    return "Local"
}

# Sendet ein Ereignis an den ESP-Anwesenheit-Monitor. Ein Fehlschlag (Geraet
# nicht erreichbar, WLAN/LAN-Aussetzer, ESP gerade im OTA-Neustart) wird nach
# einem Retry nur geloggt, nicht weiter eskaliert - der Server haelt seinen
# Status ohnehin nur im RAM, ein verlorenes Ereignis faellt beim naechsten
# Wechsel (spaetestens beim naechsten Login) automatisch wieder in einen
# konsistenten Zustand.
function Send-AnwesenheitEvent {
    param(
        [Parameter(Mandatory)][string]$EventType,
        [string]$LogonType = ""
    )
    $payload = @{
        computer  = $env:COMPUTERNAME
        user      = $env:USERNAME
        event     = $EventType
        logontype = $LogonType
        timestamp = (Get-Date -Format "yyyy-MM-ddTHH:mm:ss")
    } | ConvertTo-Json -Compress

    for ($attempt = 1; $attempt -le 2; $attempt++) {
        try {
            Invoke-RestMethod -Uri $script:ServerUrl -Method Post -Body $payload `
                -ContentType "application/json" -TimeoutSec 5 | Out-Null
            Write-AgentLog "Gesendet: $EventType/$LogonType"
            return
        } catch {
            if ($attempt -ge 2) {
                Write-AgentLog "FEHLER beim Senden von $EventType : $($_.Exception.Message)"
            } else {
                Start-Sleep -Seconds 2
            }
        }
    }
}

# Ordnet SessionSwitchReason auf die 7 in der Projektbeschreibung
# festgelegten Ereignistypen ab. ConsoleConnect/ConsoleDisconnect (lokaler
# Benutzerwechsel per "Benutzer wechseln" an der Konsole) werden bewusst
# NICHT gemeldet - sie gehoeren nicht zu den 7 definierten Ereignistypen, und
# der Server (EventManager::mapEventToState) wuerde einen unbekannten
# event-Wert ohnehin mit HTTP 400 ablehnen.
$script:SessionSwitchHandler = {
    param($senderObj, $e)
    switch ($e.Reason) {
        ([Microsoft.Win32.SessionSwitchReason]::SessionLogon) {
            Send-AnwesenheitEvent -EventType "login" -LogonType (Get-CurrentLogonType)
        }
        ([Microsoft.Win32.SessionSwitchReason]::SessionLogoff) {
            Send-AnwesenheitEvent -EventType "logout"
        }
        ([Microsoft.Win32.SessionSwitchReason]::SessionLock) {
            Send-AnwesenheitEvent -EventType "lock"
        }
        ([Microsoft.Win32.SessionSwitchReason]::SessionUnlock) {
            Send-AnwesenheitEvent -EventType "unlock" -LogonType (Get-CurrentLogonType)
        }
        ([Microsoft.Win32.SessionSwitchReason]::RemoteConnect) {
            Send-AnwesenheitEvent -EventType "switch-to-rdp" -LogonType "RDP"
        }
        ([Microsoft.Win32.SessionSwitchReason]::RemoteDisconnect) {
            Send-AnwesenheitEvent -EventType "rdp-disconnect" -LogonType "RDP"
        }
        default {
            # ConsoleConnect/ConsoleDisconnect - siehe Kommentar oben, bewusst ignoriert.
        }
    }
}

# Microsoft.Win32.SystemEvents unterhaelt intern einen eigenen Thread mit
# versteckter Nachrichtenschleife (genau dafuer entworfen, damit auch
# Konsolen-Apps/Dienste ohne eigene WinForms-Message-Loop Sitzungsereignisse
# empfangen koennen) - kein eigenes Application.Run() noetig.
[Microsoft.Win32.SystemEvents]::add_SessionSwitch($script:SessionSwitchHandler)

Write-AgentLog "Agent gestartet (Server: $script:ServerUrl, Benutzer: $env:USERNAME)"

# Initialer Login-Event: der Scheduled Task startet SELBST erst durch die
# Anmeldung (Trigger "Bei Anmeldung", siehe Install-AnwesenheitAgent.ps1) -
# das zugehoerige SessionLogon-Ereignis liegt zu diesem Zeitpunkt bereits in
# der Vergangenheit und wuerde ohne diesen expliziten Aufruf nie gemeldet.
Send-AnwesenheitEvent -EventType "login" -LogonType (Get-CurrentLogonType)

try {
    while ($true) {
        Start-Sleep -Seconds 60
    }
} finally {
    [Microsoft.Win32.SystemEvents]::remove_SessionSwitch($script:SessionSwitchHandler)
    Write-AgentLog "Agent beendet"
}
