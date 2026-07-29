# ESP-Anwesenheit

Zentraler Windows-Login-Monitor auf einem WT32-ETH01 (ESP32 mit Ethernet).

## Zweck

**Auf einen Blick sehen: wer sitzt gerade an welchem PC - und welcher
Arbeitsplatz ist gerade frei.**

Jeder überwachte Windows-PC meldet seine Sitzungsereignisse (Anmeldung,
Abmeldung, Sperren, Entsperren, RDP-Verbindung/-Trennung) an einen zentralen
ESP32. Der sammelt daraus zwei zusammengehörige Sichten:

- **Wer arbeitet gerade wo?** - aktueller Benutzer je Rechner, live in der
  Weboberfläche.
- **Welche Arbeitsplätze sind frei?** - Rechner ohne aktive Sitzung
  ("Loginmaske") sind auf denselben Blick sofort erkennbar, ohne jeden PC
  einzeln abzulaufen.

Kein Agent auf dem ESP32 nötig, kein zentraler Server außer dem Board selbst -
die gesamte Logik läuft lokal im Netzwerk (LAN/WLAN), keine Cloud-Anbindung.

## Architektur

```
Windows-PC 1 ─┐
Windows-PC 2 ─┼─ POST /event (JSON) ─▶  ESP32 (WT32-ETH01)  ─▶  Web-UI / SNMP
Windows-PC N ─┘                          (RAM-Ringpuffer,
                                          14-Tage-CSV-Historie)
```

- **`firmware/`** - PlatformIO-Projekt für den WT32-ETH01: HTTP-API
  (`/event`, `/status`, `/events`), Live-Web-UI (AJAX-Live-Update),
  Einstellungsseite (Auth, Netzwerk mit LAN/WLAN-Fallback, OTA-Update,
  Werksreset), SNMP v1/v2c (read-only) für Zabbix & Co.
- **`client/windows/`** - PowerShell-Agent, der auf jedem überwachten PC
  läuft (Scheduled Task, keine Installation als Dienst nötig) und
  Sitzungswechsel per `Microsoft.Win32.SystemEvents.SessionSwitch` erkennt.
- **`docs/`** - Entscheidungsdokumentation, Flash-Anleitung, Zabbix-Template.

## Features

- Live-Status aller bekannten Rechner + Ringpuffer der letzten 500 Ereignisse
- Tägliche Verdichtung zu einer 14 Tage haltenden, herunterladbaren
  `logins.csv`
- Filter nach Benutzer/Rechner, automatische Web-UI-Aktualisierung
- LAN-Vorrang mit optionalem WLAN-Fallback (inkl. eigenem Einrichtungs-
  Access-Point), NTP-Zeitsynchronisation
- Passwortgeschützte Einstellungsseite, lokales OTA-Firmware-Update, Werksreset
  (Einstellungen/Daten/alles)
- SNMP v1/v2c read-only + fertiges Zabbix-Template (Systeminfo, Netzwerk,
  angemeldete Benutzer, Uptime, freier Heap)

## Schnellstart

1. Firmware flashen: [`docs/flash-anleitung.txt`](docs/flash-anleitung.txt)
   (Pinbelegung, Verkabelung) + [`scripts/flash.ps1`](scripts/flash.ps1)
   (Windows, baut & flasht automatisch)
2. Windows-Client auf jedem zu überwachenden PC installieren:
   `client/windows/Install-AnwesenheitAgent.ps1 -ServerUrl "http://<ESP-IP>/event"`
   (als Administrator)
3. Weboberfläche des ESP32 öffnen (`http://<ESP-IP>/` bzw.
   `http://esp-anwesenheit.local/`)

Hintergrund und Design-Entscheidungen: [`docs/entscheidungen.md`](docs/entscheidungen.md).
Ursprüngliche Anforderungen: [`Projektbeschreibung.txt`](Projektbeschreibung.txt).

## Status

Firmware und Client sind gebaut und softwareseitig verifiziert (`pio run`,
PowerShell-Syntaxprüfung, End-to-End-Test des Event-JSON gegen einen
Mock-Server) - **noch nicht auf echter Hardware / im echten Anmeldezyklus
getestet.**

## Tags

`esp32` `wt32-eth01` `iot` `presence-monitoring` `workplace-occupancy`
`login-monitor` `session-monitoring` `snmp` `zabbix` `powershell` `windows`
`platformio` `arduino` `ethernet` `home-network` `self-hosted`
