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

    Serveradresse: ist in anwesenheit-client.json (vom Installer angelegt) eine
    "serverUrl" gesetzt, wird diese verwendet. Ist sie leer/fehlt (Auto-Modus,
    der neue Installer-Default), findet der Agent den ESP per UDP-Broadcast-
    Discovery selbst (Invoke-EspDiscovery, Port 55321 - muss zur Firmware
    passen), cached das Ergebnis in %LOCALAPPDATA% und sucht bei wiederholten
    Sendefehlern erneut (robust gegen DHCP-IP-Wechsel). Letzter Ausweg, falls
    kein Geraet antwortet: der mDNS-Name http://esp-anwesenheit.local/event.

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
    numerische Sitzungs-ID ergaenzt (z.B. "RDSHOST01 (Sitzung 3)"), damit
    jede Sitzung eine eigene Zeile in der Weboberflaeche bekommt statt sich
    gegenseitig zu ueberschreiben. Auf normalem Client-Windows (Win10/11,
    immer nur eine Sitzung gleichzeitig) bleibt der reine Rechnername
    unveraendert - siehe Get-ComputerIdentifier.
#>

Add-Type -AssemblyName System.Windows.Forms

# Erhoeht die Shutdown-Prioritaet dieses Prozesses (Standard: mittig
# zwischen 0x0 und 0x3FF) - Windows beendet Prozesse mit hoeherer
# Prioritaet (naeher an 0x3FF) beim Herunterfahren/Abmelden/Neustart
# tendenziell SPAETER, was dem "logout"-Handler etwas mehr Zeit fuer
# seinen HTTP-POST verschafft. 0x3FF ist die hoechste fuer normale
# Anwendungen vorgesehene Stufe (0x400+ ist Systemprozessen vorbehalten).
# Keine Garantie - bei einem durch Windows Update ausgeloesten Neustart
# (real beobachtet: kein Logout kam an, siehe docs/entscheidungen.md)
# kann das Zeitfenster trotzdem zu kurz sein.
try {
    Add-Type -Namespace EspAnwesenheit -Name NativeMethods -MemberDefinition @"
[DllImport("kernel32.dll", SetLastError = true)]
public static extern bool SetProcessShutdownParameters(uint dwLevel, uint dwFlags);
"@ -ErrorAction Stop
    [EspAnwesenheit.NativeMethods]::SetProcessShutdownParameters(0x3FF, 0) | Out-Null
} catch {
    # Best effort - falls das aus irgendeinem Grund fehlschlaegt, laeuft der
    # Agent mit normaler (unveraenderter) Shutdown-Prioritaet weiter.
}

$script:ScriptDir = if ($PSScriptRoot) { $PSScriptRoot } else { Split-Path -Parent $MyInvocation.MyCommand.Path }
$script:ConfigPath = Join-Path $script:ScriptDir "anwesenheit-client.json"

# %LOCALAPPDATA% statt neben dem Skript (das liegt typischerweise unter
# C:\Program Files\, siehe .DESCRIPTION oben) - der Agent laeuft absichtlich
# ohne Admin-Rechte und kann dort nicht schreiben.
$script:LogDir = Join-Path $env:LOCALAPPDATA "ESP-Anwesenheit"
New-Item -ItemType Directory -Path $script:LogDir -Force -ErrorAction SilentlyContinue | Out-Null
$script:LogPath = Join-Path $script:LogDir "client.log"
$script:MaxLogBytes = 1MB
# Cache der zuletzt per UDP-Discovery gefundenen URL - bewusst in %LOCALAPPDATA%
# (nicht neben dem Skript unter Programme, wo der eingeschraenkte Benutzer nicht
# schreiben darf). Dient als Warmstart-/Fallback, wenn eine Discovery-Runde mal
# ohne Antwort bleibt (z.B. Geraet gerade im Neustart).
$script:CachePath = Join-Path $script:LogDir "discovered-url.txt"

# Explizit vom Installer gesetzte Server-URL (anwesenheit-client.json im
# Installationsverzeichnis). Leer/fehlend => Auto-Discovery-Modus (siehe
# Resolve-ServerUrl weiter unten). Die eigentliche URL-Aufloesung passiert erst
# NACH Write-AgentLog/Invoke-EspDiscovery, weil sie diese benutzt.
function Get-ConfiguredServerUrl {
    if (Test-Path $script:ConfigPath) {
        try {
            $cfg = Get-Content $script:ConfigPath -Raw | ConvertFrom-Json
            if ($cfg.serverUrl) { return [string]$cfg.serverUrl }
        } catch {
            # ungueltiges JSON -> wie "nicht gesetzt" behandeln (Auto-Discovery)
        }
    }
    return ""
}

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

# --- UDP-Auto-Discovery ------------------------------------------------------
# Findet den ESP-Anwesenheit-Monitor ohne fest eingetragene IP: ein Broadcast
# mit der Anfrage-Kennung an den Discovery-Port, das Geraet antwortet per
# Unicast mit einem JSON (ip/port/path). Port + Kennung MUESSEN mit der Firmware
# uebereinstimmen (firmware/src/main.cpp: kDiscoveryPort/kDiscoveryRequest).
$script:DiscoveryPort = 55321
$script:DiscoveryRequest = "ESP-ANWESENHEIT-DISCOVERY?"

# Broadcast-Ziele: gerichtete Broadcast-Adresse jedes aktiven IPv4-Interfaces
# (aus IP + Subnetzmaske) plus die limitierte Broadcast-Adresse. Deckt auf einem
# Mehr-NIC-PC alle lokalen Subnetze ab.
function Get-BroadcastTargets {
    $set = New-Object System.Collections.Generic.List[string]
    $set.Add("255.255.255.255") | Out-Null
    try {
        foreach ($ni in [System.Net.NetworkInformation.NetworkInterface]::GetAllNetworkInterfaces()) {
            if ($ni.OperationalStatus -ne [System.Net.NetworkInformation.OperationalStatus]::Up) { continue }
            if ($ni.NetworkInterfaceType -eq [System.Net.NetworkInformation.NetworkInterfaceType]::Loopback) { continue }
            foreach ($ua in $ni.GetIPProperties().UnicastAddresses) {
                if ($ua.Address.AddressFamily -ne [System.Net.Sockets.AddressFamily]::InterNetwork) { continue }
                if (-not $ua.IPv4Mask) { continue }
                $ipB = $ua.Address.GetAddressBytes()
                $mB  = $ua.IPv4Mask.GetAddressBytes()
                if ($mB.Length -ne 4 -or (($mB[0] -bor $mB[1] -bor $mB[2] -bor $mB[3]) -eq 0)) { continue }
                $bc = New-Object 'System.Byte[]' 4
                for ($i = 0; $i -lt 4; $i++) { $bc[$i] = [byte]($ipB[$i] -bor (0xFF -bxor $mB[$i])) }
                $addr = (New-Object System.Net.IPAddress (, $bc)).ToString()
                if (-not $set.Contains($addr)) { $set.Add($addr) | Out-Null }
            }
        }
    } catch { }
    return $set
}

# Sendet den Discovery-Broadcast und wartet auf die Antwort des Geraets.
# Rueckgabe: fertige Event-URL (z.B. http://192.168.1.50/event) oder $null.
function Invoke-EspDiscovery {
    param([int]$TimeoutMs = 1500, [int]$Attempts = 3)
    $reqBytes = [System.Text.Encoding]::ASCII.GetBytes($script:DiscoveryRequest)
    $targets = Get-BroadcastTargets
    for ($a = 1; $a -le $Attempts; $a++) {
        $udp = $null
        try {
            $udp = New-Object System.Net.Sockets.UdpClient
            $udp.EnableBroadcast = $true
            foreach ($t in $targets) {
                try { [void]$udp.Send($reqBytes, $reqBytes.Length, $t, $script:DiscoveryPort) } catch { }
            }
            $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMs)
            while ($true) {
                $remainMs = [int]($deadline - [DateTime]::UtcNow).TotalMilliseconds
                if ($remainMs -lt 1) { break }
                $udp.Client.ReceiveTimeout = $remainMs
                $remote = New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Any, 0)
                try { $data = $udp.Receive([ref]$remote) } catch { break }  # Timeout/Fehler
                try { $obj = ([System.Text.Encoding]::ASCII.GetString($data)) | ConvertFrom-Json } catch { continue }
                if ($obj.service -eq "esp-anwesenheit" -and $obj.ip) {
                    $port = if ($obj.port) { [int]$obj.port } else { 80 }
                    $path = if ($obj.path) { [string]$obj.path } else { "/event" }
                    $hostpart = if ($port -eq 80) { [string]$obj.ip } else { "$($obj.ip):$port" }
                    return "http://$hostpart$path"
                }
            }
        } catch {
        } finally {
            if ($udp) { $udp.Close() }
        }
    }
    return $null
}

function Save-DiscoveredUrl {
    param([string]$Url)
    try { Set-Content -Path $script:CachePath -Value $Url -Encoding UTF8 -ErrorAction Stop } catch { }
}
function Get-CachedUrl {
    if (Test-Path $script:CachePath) {
        try {
            $u = (Get-Content $script:CachePath -Raw -ErrorAction Stop).Trim()
            if ($u) { return $u }
        } catch { }
    }
    return $null
}

# Ermittelt die zu verwendende Server-URL:
#   1. explizit im Installer gesetzt   -> diese (Admin hat die Kontrolle)
#   2. sonst Auto-Discovery per Broadcast (+ Ergebnis cachen)
#   3. sonst zuletzt gecachte Discovery-URL
#   4. sonst mDNS-Hostname als letzter Ausweg
function Resolve-ServerUrl {
    if (-not $script:AutoMode) { return $script:ConfiguredUrl }
    $found = Invoke-EspDiscovery
    if ($found) { Write-AgentLog "ESP per UDP-Discovery gefunden: $found"; Save-DiscoveredUrl $found; return $found }
    $cached = Get-CachedUrl
    if ($cached) { Write-AgentLog "UDP-Discovery ohne Antwort - nutze gecachte URL: $cached"; return $cached }
    Write-AgentLog "UDP-Discovery ohne Antwort - Fallback auf mDNS-Namen"
    return "http://esp-anwesenheit.local/event"
}

$script:ConfiguredUrl = Get-ConfiguredServerUrl
$script:AutoMode = [string]::IsNullOrWhiteSpace($script:ConfiguredUrl)
$script:ServerUrl = Resolve-ServerUrl
# Zaehlt aufeinanderfolgende Sendefehler; loest im Auto-Modus eine erneute
# Discovery aus (macht den Client robust gegen DHCP-IP-Wechsel des Geraets).
$script:ConsecutiveFailures = 0

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
#
# KORREKTUR (2026-07-30, real auf einem echten RDS-Host bestaetigt):
# urspruenglich wurde $env:SESSIONNAME als Sitzungskennung angehaengt
# ("Console" bzw. "RDP-Tcp#<n>") - real beobachtet blieb diese aber LEER,
# wenn der Agent vom Scheduled-Task-Dienst gestartet wird (der Dienst baut
# sein eigenes Umgebungsblock aus dem gespeicherten Benutzerprofil auf und
# uebernimmt NICHT die von winlogon/explorer zur Laufzeit dynamisch
# gesetzten Sitzungsvariablen einer interaktiven Anmeldung) - die
# Sitzungskennung fiel dadurch komplett weg, obwohl ProductType korrekt
# als Server erkannt wurde. Stattdessen jetzt die numerische Sitzungs-ID
# direkt vom Betriebssystem abgefragt (.NET
# Process.GetCurrentProcess().SessionId) - eine echte, vom Betriebssystem
# geloest zurueckgelieferte Prozesseigenschaft, unabhaengig davon, wie/von
# wem der Prozess gestartet wurde. Weniger "huebsch" als "RDP-Tcp#3" (nur
# eine Zahl), dafuer zuverlaessig immer vorhanden.
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
    $sessionId = [System.Diagnostics.Process]::GetCurrentProcess().SessionId
    return "$env:COMPUTERNAME (Sitzung $sessionId)"
}
# Einmal beim Start ermittelt (aendert sich waehrend der Laufzeit einer
# Sitzung nicht) statt bei jedem Send-AnwesenheitEvent neu abgefragt.
$script:ComputerIdentifier = Get-ComputerIdentifier

# Aktueller Sitzungszustand aus Serversicht ("Lokal"/"RDP"/"Gesperrt"), vom
# Client mitgefuehrt und bei jedem Sitzungswechsel unten aktualisiert. Der
# periodische Heartbeat sendet diesen Zustand mit, damit der Server die
# Sitzung "frisch" haelt (sonst blendet die Weboberflaeche sie nach ~15 min
# als "stale" aus und die Firmware entfernt sie nach staleEntryHours) - und
# sie nach einem Geraete-Reboot automatisch wiederherstellt. Leer = kein
# aktiver Zustand (z.B. nach logout) -> es wird kein Heartbeat gesendet.
$script:CurrentState = ""
$script:HeartbeatIntervalSec = 300  # alle 5 min (UI-Stale-Schwelle liegt bei 15 min)

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
        [string]$State = "",
        [int]$TimeoutSec = 5,
        [int]$MaxAttempts = 2
    )
    $body = @{
        computer  = $script:ComputerIdentifier
        user      = $env:USERNAME
        event     = $EventType
        logontype = $LogonType
        timestamp = (Get-Date -Format "yyyy-MM-ddTHH:mm:ss")
    }
    # Nur fuer "heartbeat" relevant: der Server frischt damit lastUpdate der
    # Sitzung auf (gegen das Stale-Ausblenden) bzw. stellt sie nach einem
    # Geraete-Reboot wieder her - ohne einen Historie-Eintrag zu erzeugen
    # (siehe EventManager::heartbeat in der Firmware).
    if ($State) { $body.state = $State }
    $payload = $body | ConvertTo-Json -Compress

    for ($attempt = 1; $attempt -le $MaxAttempts; $attempt++) {
        try {
            Invoke-RestMethod -Uri $script:ServerUrl -Method Post -Body $payload `
                -ContentType "application/json" -TimeoutSec $TimeoutSec | Out-Null
            Write-AgentLog "Gesendet: $EventType/$LogonType"
            $script:ConsecutiveFailures = 0
            return
        } catch {
            if ($attempt -ge $MaxAttempts) {
                Write-AgentLog "FEHLER beim Senden von $EventType : $($_.Exception.Message)"
                $script:ConsecutiveFailures++
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
            $lt = Get-CurrentLogonType
            $script:CurrentState = if ($lt -eq "RDP") { "RDP" } else { "Lokal" }
            Send-AnwesenheitEvent -EventType "login" -LogonType $lt
        }
        ([Microsoft.Win32.SessionSwitchReason]::SessionLogoff) {
            # Kurzer Timeout, kein Retry - siehe Kommentar an Send-AnwesenheitEvent.
            $script:CurrentState = ""   # abgemeldet -> ab jetzt kein Heartbeat mehr
            Send-AnwesenheitEvent -EventType "logout" -TimeoutSec 2 -MaxAttempts 1
        }
        ([Microsoft.Win32.SessionSwitchReason]::SessionLock) {
            $script:CurrentState = "Gesperrt"
            Send-AnwesenheitEvent -EventType "lock"
        }
        ([Microsoft.Win32.SessionSwitchReason]::SessionUnlock) {
            $lt = Get-CurrentLogonType
            $script:CurrentState = if ($lt -eq "RDP") { "RDP" } else { "Lokal" }
            Send-AnwesenheitEvent -EventType "unlock" -LogonType $lt
        }
        ([Microsoft.Win32.SessionSwitchReason]::RemoteConnect) {
            $script:CurrentState = "RDP"
            Send-AnwesenheitEvent -EventType "switch-to-rdp" -LogonType "RDP"
        }
        ([Microsoft.Win32.SessionSwitchReason]::RemoteDisconnect) {
            # Lokale Weiterbenutzung an der Konsole ist die naechste Annahme -
            # deckt sich mit EventManager::mapEventToState (rdp-disconnect -> Lokal).
            $script:CurrentState = "Lokal"
            Send-AnwesenheitEvent -EventType "rdp-disconnect" -LogonType "RDP"
        }
        default {
            # ConsoleConnect/ConsoleDisconnect - siehe Kommentar oben, bewusst ignoriert.
        }
    }
}

# Zusaetzlicher, fruehstmoeglicher Trigger fuer denselben Fall wie
# SessionLogoff oben (Abmeldung/Herunterfahren/Neustart) - SessionEnding
# wird ueber WM_QUERYENDSESSION ausgeloest und kann etwas frueher im
# Shutdown-Ablauf ankommen als der SessionSwitch-Reason SessionLogoff.
# Beide Trigger koennen fuer denselben echten Vorgang feuern - das ist
# bewusst in Kauf genommen (ein doppelt gesendetes "logout" ist auf
# Serverseite folgenlos, der Status wird nur erneut auf "Loginmaske"
# gesetzt) statt zu riskieren, dass ausgerechnet der schnellere der beiden
# Wege fehlt. Real beobachtet: selbst ein durch Windows Update
# ausgeloester Server-Neustart hat mit nur dem SessionLogoff-Trigger kein
# Logout mehr durchbekommen (siehe docs/entscheidungen.md) - dieser
# zusaetzliche Trigger + die erhoehte Shutdown-Prioritaet oben sollen die
# Erfolgschance weiter verbessern, ohne eine Garantie zu sein.
$script:SessionEndingHandler = {
    param($senderObj, $e)
    Send-AnwesenheitEvent -EventType "logout" -TimeoutSec 2 -MaxAttempts 1
}

# Microsoft.Win32.SystemEvents unterhaelt intern einen eigenen Thread mit
# versteckter Nachrichtenschleife (genau dafuer entworfen, damit auch
# Konsolen-Apps/Dienste ohne eigene WinForms-Message-Loop Sitzungsereignisse
# empfangen koennen) - kein eigenes Application.Run() noetig.
[Microsoft.Win32.SystemEvents]::add_SessionSwitch($script:SessionSwitchHandler)
[Microsoft.Win32.SystemEvents]::add_SessionEnding($script:SessionEndingHandler)

Write-AgentLog "Agent gestartet (Server: $script:ServerUrl, Rechner: $script:ComputerIdentifier, Benutzer: $env:USERNAME)"

# Initialer Login-Event: der Scheduled Task startet SELBST erst durch die
# Anmeldung (Trigger "Bei Anmeldung", siehe Install-AnwesenheitAgent.ps1) -
# das zugehoerige SessionLogon-Ereignis liegt zu diesem Zeitpunkt bereits in
# der Vergangenheit und wuerde ohne diesen expliziten Aufruf nie gemeldet.
$script:InitialLogonType = Get-CurrentLogonType
$script:CurrentState = if ($script:InitialLogonType -eq "RDP") { "RDP" } else { "Lokal" }
Send-AnwesenheitEvent -EventType "login" -LogonType $script:InitialLogonType

# Periodischer Heartbeat: haelt die Sitzung serverseitig "frisch" (gegen das
# Stale-Ausblenden), stellt sie nach einem Geraete-Reboot wieder her und
# dient als zuverlaessiger Fallback fuer ein verpasstes logout (bleiben die
# Heartbeats aus, entfernt die Firmware die Sitzung nach staleEntryHours).
# Bewusst KEIN Historie-Eintrag serverseitig (siehe EventManager::heartbeat).
try {
    while ($true) {
        Start-Sleep -Seconds $script:HeartbeatIntervalSec
        # Im Auto-Modus nach wiederholten Sendefehlern (z.B. Geraet hat per DHCP
        # eine neue IP bekommen) neu suchen. Bewusst NUR hier im Heartbeat, NICHT
        # im zeitkritischen Logout-Pfad (ein Broadcast + Wartezeit waere dort zu
        # langsam).
        if ($script:AutoMode -and $script:ConsecutiveFailures -ge 2) {
            $again = Invoke-EspDiscovery
            if ($again) {
                if ($again -ne $script:ServerUrl) { Write-AgentLog "Server-URL per Re-Discovery aktualisiert: $again" }
                $script:ServerUrl = $again
                Save-DiscoveredUrl $again
                $script:ConsecutiveFailures = 0
            }
        }
        if ($script:CurrentState) {
            Send-AnwesenheitEvent -EventType "heartbeat" -State $script:CurrentState
        }
    }
} finally {
    [Microsoft.Win32.SystemEvents]::remove_SessionSwitch($script:SessionSwitchHandler)
    [Microsoft.Win32.SystemEvents]::remove_SessionEnding($script:SessionEndingHandler)
    Write-AgentLog "Agent beendet"
}
