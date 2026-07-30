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

    Log-Datei liegt bewusst NICHT neben dem Skript (typischerweise
    C:\Program Files\ESP-Anwesenheit\, siehe Install-AnwesenheitAgent.ps1),
    sondern unter %LOCALAPPDATA%\ESP-Anwesenheit\ - der Agent laeuft mit
    Absicht als eingeschraenkter Benutzer ohne Admin-Rechte (RunLevel
    Limited, siehe Installer), und Program Files ist fuer normale Benutzer
    nur lesbar, nicht beschreibbar. %LOCALAPPDATA% ist fuer den jeweils
    angemeldeten Benutzer immer beschreibbar, ganz ohne Sonderrechte -
    als Nebeneffekt bekommt auf einem RDS-Host (siehe Get-ComputerIdentifier)
    jeder Benutzer automatisch sein eigenes Log.

    Auf einem Windows-Server mit RDS-Rolle (mehrere gleichzeitige Sitzungen
    auf demselben Rechner moeglich) wird der Rechnername im Payload um die
    Sitzungskennung ergaenzt (z.B. "RDSHOST01 (RDP-Tcp#5)"), damit jede
    Sitzung eine eigene Zeile in der Weboberflaeche bekommt statt sich
    gegenseitig zu ueberschreiben. Auf normalem Client-Windows (Win10/11,
    immer nur eine Sitzung gleichzeitig) bleibt der reine Rechnername
    unveraendert - siehe Get-ComputerIdentifier.
#>

Add-Type -AssemblyName System.Windows.Forms

$script:ScriptDir = if ($PSScriptRoot) { $PSScriptRoot } else { Split-Path -Parent $MyInvocation.MyCommand.Path }
$script:ConfigPath = Join-Path $script:ScriptDir "anwesenheit-client.json"

# %LOCALAPPDATA% statt neben dem Skript (das liegt typischerweise unter
# C:\Program Files\, siehe .DESCRIPTION oben) - der Agent laeuft absichtlich
# ohne Admin-Rechte und kann dort nicht schreiben.
$script:LogDir = Join-Path $env:LOCALAPPDATA "ESP-Anwesenheit"
New-Item -ItemType Directory -Path $script:LogDir -Force -ErrorAction SilentlyContinue | Out-Null
$script:LogPath = Join-Path $script:LogDir "client.log"
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
        # -ErrorAction Stop: Add-Content wirft standardmaessig einen NICHT
        # abbrechenden Fehler - ohne dieses Flag (und ohne globales
        # $ErrorActionPreference = "Stop") wuerde das umgebende try/catch
        # ihn gar nicht erst abfangen und der Fehler liefe stattdessen in
        # den Error-Stream durch (sichtbar als rotes PowerShell-Fenster) -
        # genau so real aufgetreten, bevor der Log-Pfad ausserdem auf
        # %LOCALAPPDATA% umgestellt wurde (siehe oben).
        Add-Content -Path $script:LogPath -Value $line -Encoding UTF8 -ErrorAction Stop
    } catch {
        # Logging darf den Agenten nie zum Absturz bringen (z.B. Verzeichnis
        # schreibgeschuetzt) - Fehler hier wird bewusst verschluckt.
    }
}

function Get-CurrentLogonType {
    if ([System.Windows.Forms.SystemInformation]::TerminalServerSession) { return "RDP" }
    return "Local"
}

# Auf normalem Client-Windows (Win10/11) ist immer nur EINE interaktive
# Sitzung gleichzeitig moeglich - eine eingehende RDP-Verbindung uebernimmt
# die bestehende Konsolensitzung, statt eine zweite zu eroeffnen. Dort bleibt
# der reine Rechnername ($env:COMPUTERNAME) weiterhin eindeutig (z.B.
# "PC-25"), unveraendertes Verhalten.
#
# Auf einem Windows-Server mit RDS-Rolle (Remote Desktop Session Host) gilt
# das NICHT - dort koennen mehrere Kollegen gleichzeitig eigene Sitzungen auf
# demselben physischen Rechner haben. $env:COMPUTERNAME allein wuerde alle
# auf denselben Status-Eintrag in EventManager zusammenfallen lassen (jede
# neue Anmeldung wuerde die vorherige ueberschreiben statt eine eigene Zeile
# zu bekommen) - genau das war der Anlass fuer diese Ergaenzung. Erkennung
# ueber Win32_OperatingSystem.ProductType (1=Workstation, 2=Domain
# Controller, 3=Server) statt z.B. ueber "laeuft gerade mehr als eine
# Sitzung" - liefert ein stabiles, sofort beim Start feststehendes Ergebnis
# ohne wiederholte Sitzungsabfrage, und braucht keine erhoehten Rechte.
# $env:SESSIONNAME liefert je Sitzung einen eindeutigen Wert ("Console" bzw.
# "RDP-Tcp#<n>").
function Get-ComputerIdentifier {
    try {
        $productType = (Get-CimInstance -ClassName Win32_OperatingSystem -Property ProductType -ErrorAction Stop).ProductType
    } catch {
        # Im Zweifel (CIM nicht verfuegbar) wie Workstation behandeln -
        # unveraendertes, bisheriges Verhalten statt eines Fehlers.
        $productType = 1
    }
    if ($productType -eq 1) {
        return $env:COMPUTERNAME
    }
    if ([string]::IsNullOrEmpty($env:SESSIONNAME)) {
        return $env:COMPUTERNAME
    }
    return "$env:COMPUTERNAME ($env:SESSIONNAME)"
}
# Einmal beim Start ermittelt (aendert sich waehrend der Laufzeit einer
# Sitzung nicht) statt bei jedem Send-AnwesenheitEvent neu abgefragt.
$script:ComputerIdentifier = Get-ComputerIdentifier

# Sendet ein Ereignis an den ESP-Anwesenheit-Monitor. Ein Fehlschlag (Geraet
# nicht erreichbar, WLAN/LAN-Aussetzer, ESP gerade im OTA-Neustart) wird nach
# einem Retry nur geloggt, nicht weiter eskaliert - der Server haelt seinen
# Status ohnehin nur im RAM, ein verlorenes Ereignis faellt beim naechsten
# Wechsel (spaetestens beim naechsten Login) automatisch wieder in einen
# konsistenten Zustand.
#
# -TimeoutSec/-MaxAttempts sind bewusst ueberschreibbar: bei "logout" killt
# Windows den Scheduled-Task-Prozess waehrend Abmeldung/Herunterfahren nach
# einer kurzen Gnadenfrist (typischerweise nur wenige Sekunden) - der
# Standard-Ablauf (5s Timeout + 2s Pause + zweiter 5s-Versuch, bis zu ~12s)
# passt in dieses Fenster oft nicht mehr hinein, wodurch das Logout-Ereignis
# nie ankommt (real beobachtet: ausschliesslich login-Ereignisse in der
# Historie, nie ein logout davor). Der SessionLogoff-Handler unten ruft
# deshalb mit kurzem Timeout und ohne Retry auf - erhoeht die Chance, noch
# rechtzeitig fertig zu werden, auf Kosten der Netzwerk-Resilienz genau fuer
# diesen einen, ohnehin zeitkritischen Fall.
function Send-AnwesenheitEvent {
    param(
        [Parameter(Mandatory)][string]$EventType,
        [string]$LogonType = "",
        [int]$TimeoutSec = 5,
        [int]$MaxAttempts = 2
    )
    $payload = @{
        computer  = $script:ComputerIdentifier
        user      = $env:USERNAME
        event     = $EventType
        logontype = $LogonType
        timestamp = (Get-Date -Format "yyyy-MM-ddTHH:mm:ss")
    } | ConvertTo-Json -Compress

    for ($attempt = 1; $attempt -le $MaxAttempts; $attempt++) {
        try {
            Invoke-RestMethod -Uri $script:ServerUrl -Method Post -Body $payload `
                -ContentType "application/json" -TimeoutSec $TimeoutSec | Out-Null
            Write-AgentLog "Gesendet: $EventType/$LogonType"
            return
        } catch {
            if ($attempt -ge $MaxAttempts) {
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
            # Kurzer Timeout, kein Retry - siehe Kommentar an Send-AnwesenheitEvent.
            Send-AnwesenheitEvent -EventType "logout" -TimeoutSec 2 -MaxAttempts 1
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

Write-AgentLog "Agent gestartet (Server: $script:ServerUrl, Rechner: $script:ComputerIdentifier, Benutzer: $env:USERNAME)"

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
