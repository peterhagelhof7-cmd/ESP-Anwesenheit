# Entscheidungen - ESP-Anwesenheit

## Aufraeumen veralteter Status-Zeilen + eigener OTA-Fix (2026-07-30, v0.2.0)

Auf Nutzerwunsch: nach dem RDS-Sitzungs-Test faellt auf, dass jede neue
Sitzungs-ID (siehe oben) eine dauerhafte eigene `/status`-Zeile bekommt,
auch nach dem Logout ("Loginmaske") - bei haeufigen Reconnects auf einem
stark genutzten RDS-Host wuerden sich mit der Zeit viele tote Zeilen
ansammeln.

**Neu:** `DeviceConfig.staleEntryHours` (Default 2, 0 = deaktiviert,
Einstellungsseite "Anzeige"). `EventManager::pruneStaleStatuses()`
entfernt Status-Zeilen, deren `lastUpdate` laenger als die konfigurierte
Stundenzahl zurueckliegt - bewusst UNABHAENGIG vom `state` (auch
"Lokal"/"RDP"/"Gesperrt", nicht nur "Loginmaske"): ein Rechner, der sich
einfach nie wieder meldet (aus, Agent deinstalliert, dauerhafter
Netzwerkausfall), soll ebenso verschwinden, nicht nur ausdrueckliche
Abmeldungen. Wie bei `HistoryManager::pruneOldRows()` wird ohne
synchronisierte Uhr (`isTimeSynced()`) NICHT geprueft - sonst koennte
eine falsch gehende RTC kurz nach dem Boot versehentlich alles loeschen.
Aufruf alle 5 Minuten aus `main.cpp::loop()` (kein neuer Manager noetig,
analog zum bestehenden mDNS-Start-Muster) statt bei jedem Loop-Durchlauf -
die Pruefung selbst ist zwar billig, aber unnoetig bei einer
Stunden-Schwelle.

**Zusaetzlich mitgezogen:** derselbe `UPDATE_SIZE_UNKNOWN`-OTA-Bug wie bei
sensormeter-wlan (siehe dortiges `docs/entscheidungen.md`) steckte auch
hier im OTA-Upload-Handler - identischer Fix (`r->contentLength()` statt
`UPDATE_SIZE_UNKNOWN`), noch vor dem geplanten Live-OTA-Test dieses
Updates behoben, um die Erfolgschance des Uploads selbst zu erhoehen.

Version auf 0.2.0 angehoben (neues Feature, siehe `config.h`/`config.h.example`).
`pio run` erfolgreich (Flash 80,5%, RAM 28,9%), Version im Binary per
`SM-FW-ID:ESP-ANWESENHEIT:0.2.0:SM-FW-END` bestaetigt.

**Nicht real ueber die volle Laufzeit verifiziert:** das eigentliche
Entfernen einer veralteten Zeile nach Ablauf der konfigurierten Stunden
wurde nicht live beobachtet (bräuchte mehrere Stunden Wartezeit) - Logik
per Code-Review sorgfaeltig geprueft, Einstellungsfeld/Speichern-Rundlauf
sowie der OTA-Upload selbst wurden dagegen real auf dem laufenden Geraet
getestet (siehe unten).

## RDS-Sitzungskennung: $env:SESSIONNAME leer bei Scheduled-Task-Start (2026-07-30, Nebenfrage geklaert)

Die offene Nebenfrage aus dem vorherigen Eintrag ist geklaert: Nutzer hat
die lokale `client.log` von "SRV-RDS" geschickt - `Get-ComputerIdentifier`
lieferte dort durchgehend (3x, ueber 26 Minuten verteilt, kein einmaliger
Timing-Zufall) nur den reinen Rechnernamen "SRV-RDS" ohne Sitzungskennung.

**Root Cause:** `$env:SESSIONNAME` (bisherige Quelle der Sitzungskennung,
"Console"/"RDP-Tcp#<n>") ist bei einem vom Scheduled-Task-Dienst
gestarteten Prozess NICHT zuverlaessig gesetzt - der Dienst baut sein
eigenes Umgebungsblock aus dem gespeicherten Benutzerprofil auf und
uebernimmt dabei nicht die von winlogon/explorer nur zur Laufzeit einer
interaktiven Anmeldung dynamisch gesetzten Sitzungsvariablen.
`Win32_OperatingSystem.ProductType` (die andere Hälfte der Erkennung) war
dabei nachweislich korrekt (der Server-Zweig wurde ja betreten, sonst
waere ueberhaupt keine Sitzungskennung versucht worden) - das Problem lag
ausschliesslich an der Umgebungsvariable.

**Fix:** `$env:SESSIONNAME` ersetzt durch
`[System.Diagnostics.Process]::GetCurrentProcess().SessionId` - eine
direkt vom Betriebssystem gelieferte Prozesseigenschaft, unabhaengig vom
Startkontext (Scheduled Task, interaktive Shell, o.ae.) immer korrekt
gesetzt. Format geaendert von "RDSHOST01 (RDP-Tcp#5)" auf "RDSHOST01
(Sitzung 5)" - weniger sprechend (nur eine Zahl statt des WTS-
Sitzungsnamens), dafuer zuverlaessig.

Real verifiziert: isolierter Funktionstest mit simuliertem
`ProductType=3` liefert jetzt korrekt "<COMPUTERNAME> (Sitzung
<SessionId>)"; `ProductType=1` weiterhin unveraendert nur
`<COMPUTERNAME>`. Der eigentliche Beweis (dass es auf einem echten
Scheduled-Task-Start eines RDS-Hosts jetzt tatsaechlich greift) steht
noch aus - der naechste reale Test auf "SRV-RDS" sollte das zeigen.

## AnwesenheitAgent.ps1: Logout weiterhin nicht angekommen, auch nach Windows-Update-Neustart (2026-07-30, Folgefix)

Nach dem vorherigen Fix (kurzer Timeout fuer "logout") real gegengeprueft:
Nutzer hat auf einem echten Windows Server mit RDS-Rolle ("SRV-RDS") getestet
und den Server ueber Windows Update in den Neustart geschickt - server-
seitig kam weiterhin KEIN `logout`/`rdp-disconnect` an, nur die beiden
vorherigen Logins. Ein durch Windows Update ausgeloester Neustart ist ein
noch haerterer Fall als eine normale interaktive Abmeldung (typischerweise
weniger Kulanzzeit fuer laufende Benutzerprozesse).

**Zwei weitere Verbesserungen (kein Anspruch auf vollstaendige Loesung):**

1. `SetProcessShutdownParameters(0x3FF, 0)` (P/Invoke auf `kernel32.dll`,
   `-ErrorAction Stop` mit leerem Catch als Fallback) - erhoeht die
   Shutdown-Prioritaet des Agent-Prozesses auf die hoechste fuer normale
   Anwendungen vorgesehene Stufe. Windows beendet Prozesse mit hoeherer
   Prioritaet beim Herunterfahren tendenziell spaeter.
2. Zusaetzlicher Trigger auf `Microsoft.Win32.SystemEvents.SessionEnding`
   (WM_QUERYENDSESSION) NEBEN dem bisherigen `SessionSwitch`/`SessionLogoff`
   - kann etwas frueher im Shutdown-Ablauf ankommen. Beide Handler senden
   unabhaengig voneinander "logout" mit kurzem Timeout/ohne Retry; ein
   doppelt gesendetes Logout ist serverseitig folgenlos (Status wird nur
   erneut auf "Loginmaske" gesetzt).

Real verifiziert (isoliert, nicht der volle Shutdown-Fall): der
P/Invoke-Aufruf liefert `True` zurueck, `add_SessionEnding`/
`remove_SessionEnding` lassen sich ohne Fehler registrieren/entfernen.
**Nicht verifizierbar ohne einen echten Shutdown-Test**, ob das reale
Zeitfenster jetzt ausreicht - insbesondere bei einem Windows-Update-
Neustart bleibt das ungewiss, da der Update-Orchestrator eigene,
teils deutlich kuerzere Timeouts fuer laufende Benutzerprozesse
durchsetzen kann, die auch eine erhoehte Shutdown-Prioritaet nicht
zuverlaessig aushebelt.

**Offene Nebenfrage:** "SRV-RDS" erschien in den Server-Events OHNE die
erwartete Sitzungskennung in Klammern (z.B. "SRV-RDS (RDP-Tcp#3)"), obwohl
der Nutzer bestaetigt hat, dass es sich um einen echten Windows Server mit
RDS-Rolle handelt - laut `Get-ComputerIdentifier` (siehe dort) haette das
`ProductType != 1` sein und damit die Sitzungskennung anhaengen muessen.
Ursache noch nicht geklaert (CIM-Abfrage fehlgeschlagen und auf Workstation-
Verhalten zurueckgefallen? `$env:SESSIONNAME` leer? echter Bug?) - noetig
waere der genaue Inhalt der lokalen `client.log`
(`%LOCALAPPDATA%\ESP-Anwesenheit\client.log`), die "Agent gestartet
(... Rechner: ...)" mit dem tatsaechlich ermittelten Wert protokolliert,
um das einzugrenzen.

## AnwesenheitAgent.ps1: Log-Pfad + try/catch-Bug (2026-07-30, Bug real gemeldet, Folgefehler nach der GroupId-Korrektur)

Nach dem GroupId-Fix (siehe unten) lief der Scheduled Task erstmals
wirklich an - dabei zeigte sich ein sichtbares PowerShell-Fenster mit:
`Add-Content : Der Zugriff auf den Pfad "C:\Program
Files\ESP-Anwesenheit\client.log" wurde verweigert.`

**Zwei Bugs, beide bestaetigt:**

1. **Log-Pfad in Program Files ist grundsaetzlich falsch.** Der Agent
   laeuft absichtlich mit `RunLevel Limited` (kein Admin-Kontext, siehe
   Installer) - `C:\Program Files\...` ist fuer normale Benutzer aber nur
   lesbar, nicht beschreibbar. Der Log-Schreibversuch musste dort
   zwangslaeufig fehlschlagen, unabhaengig von jeder Fehlerbehandlung.
2. **`try/catch` um `Add-Content` griff gar nicht.** `Add-Content` wirft
   standardmaessig einen NICHT abbrechenden Fehler; ohne
   `-ErrorAction Stop` (und ohne globales
   `$ErrorActionPreference = "Stop"`, das dieses Skript anders als die
   beiden anderen Client-Skripte nie gesetzt hatte) faengt ein normales
   `try/catch` diesen Fehlertyp NICHT ab - er laeuft stattdessen in den
   Error-Stream durch, sichtbar als rotes PowerShell-Fenster. Real
   reproduziert: derselbe `Add-Content`-Aufruf auf einen unerreichbaren
   Pfad wirft mit `-ErrorAction Stop` einen sauber fangbaren Fehler, ohne
   fluescht er unabgefangen durch.

**Fix:**
- Log-Datei liegt jetzt unter `%LOCALAPPDATA%\ESP-Anwesenheit\client.log`
  statt neben dem Skript - fuer den jeweils angemeldeten Benutzer immer
  beschreibbar, keine Sonderrechte noetig. Nebeneffekt: auf einem
  RDS-Host (siehe `Get-ComputerIdentifier`) bekommt automatisch jeder
  Benutzer sein eigenes Log statt sich eins zu teilen/zu ueberschreiben.
- `Add-Content ... -ErrorAction Stop` ergaenzt, damit das umgebende
  `try/catch` tatsaechlich greift (bewusstes Verschlucken von
  Log-Schreibfehlern war schon immer die Absicht, hat nur nie funktioniert).

**Real verifiziert (nicht nur gelesen):** derselbe `Add-Content`-Aufruf
ohne `-ErrorAction Stop` gegen einen unerreichbaren Pfad reproduziert das
gemeldete Symptom 1:1 (Fehler sichtbar, `catch` greift nicht); mit
`-ErrorAction Stop` wird er sauber gefangen; ein Schreibversuch nach
`%LOCALAPPDATA%\ESP-Anwesenheit\` gelingt ohne Adminrechte anstandslos.

## Install-AnwesenheitAgent.ps1: GroupId per SID statt Klartextname (2026-07-30, Bug real gemeldet)

Ein Nutzer meldete beim Ausfuehren von `Install-AnwesenheitAgent.ps1`:
"Zuordnungen von Kontennamen und Sicherheitskennungen wurden nicht
durchgefuehrt" (HRESULT 0x80070534) bei `Register-ScheduledTask`.

Ursache: `New-ScheduledTaskPrincipal -GroupId "BUILTIN\Users"` verwendete
den englischen Klartextnamen der Gruppe. Auf nicht-englischen Windows-
Installationen ist das lokal anders benannt (auf Deutsch z.B.
"VORDEFINIERT\Benutzer") - der englische String laesst sich dort nicht
in eine Sicherheitskennung (SID) aufloesen. Bestaetigt am eigenen System
(deutsche Windows-Lokalisierung, `de-DE`):
`[System.Security.Principal.SecurityIdentifier]::new("S-1-5-32-545").Translate([System.Security.Principal.NTAccount])`
liefert hier `VORDEFINIERT\Benutzer`, nicht `BUILTIN\Users`.

Fix: `-GroupId "S-1-5-32-545"` (die wohlbekannte, sprachunabhaengige SID
fuer BUILTIN\Users) statt des Klartextnamens - SIDs sind unabhaengig von
der Systemsprache.

**Testabdeckung/Einschraenkung:** `New-ScheduledTaskPrincipal -GroupId
"S-1-5-32-545" -RunLevel Limited` wurde real ausgefuehrt und erstellt
das Principal-Objekt anstandslos (loest lokal zu `GroupId: Benutzer`
auf) - der eigentliche `Register-ScheduledTask`-Aufruf selbst konnte in
dieser Sitzung NICHT End-to-End getestet werden (keine Administrator-
Rechte verfuegbar, Skript braucht `#Requires -RunAsAdministrator`).
Root-Cause-Diagnose und Fix-Ansatz (SID statt Klartextname) sind ein
etabliertes, bekanntes Windows-Verhalten, aber die tatsaechliche
Task-Registrierung mit dem neuen Wert sollte beim naechsten Lauf auf
einem admin-faehigen System bestaetigt werden.

## flash.ps1 klont sich jetzt selbst (2026-07-30, Bug real gemeldet)

Ein Nutzer meldete beim Ausfuehren von `flash.ps1` den Fehler
"firmware/platformio.ini nicht gefunden unter C:\Users\<name>\firmware" -
Ursache: nur die einzelne `flash.ps1`-Datei wurde heruntergeladen (z.B. per
Rechtsklick "Speichern unter" direkt von GitHub), nicht der gesamte
Checkout. Das Skript ging bis dahin unbedingt davon aus, in `scripts/`
innerhalb eines vollstaendigen `git clone` zu liegen (`$RepoRoot =
Split-Path -Parent $PSScriptRoot`) und brach sonst nur mit einer
Fehlermeldung ab, ohne selbst etwas dagegen zu tun.

Fix (v1.1.0): findet das Skript keinen Checkout eine Ebene ueber sich
(`firmware/platformio.ini` fehlt), klont es das Repository selbst nach
`-RepoPath` (Default: Ordner "ESP-Anwesenheit" neben dem Skript) - Muster
1:1 aus `sensormeter/repo/scripts/flash.ps1` uebernommen (dort seit der
allerersten Fassung so geloest, siehe dortiges docs/entscheidungen.md).
Liegt bereits ein Checkout vor, wird bei sauberem Arbeitsstand automatisch
`git pull` versucht (uebersprungen bei lokalen Aenderungen, um nichts zu
ueberschreiben) - ebenfalls aus dem sensormeter-Muster uebernommen.

Real verifiziert: `flash.ps1` in ein leeres Verzeichnis kopiert (nur diese
eine Datei, exakt das gemeldete Szenario nachgestellt) und mit
`-SkipUpload` ausgefuehrt - klont das Repo automatisch, legt config.h an,
Build erfolgreich (Flash 80,3%, RAM 28,9%).

## Flash-Anleitung + Flash-Skript (2026-07-29)

`docs/flash-anleitung.txt` (reine Textform, auf Nutzerwunsch) + `scripts/flash.ps1`
ergaenzt. Pinbezeichnungen direkt aus dem mitgelieferten Foto
`WT32-ETH01-back.png` (Rueckseite, Aufdruck "ESP32-ETH01 V1.4") abgelesen,
nicht aus generischen Online-Pinouts uebernommen - Board hat zwei Spalten
Pins: links `TXO/RXO/IO0/IO39/IO36/IO15/IO14/IO12/IO35/IO4/IO2/GND`, rechts
`EN/GND/3V3/EN/CFG/485_EN/RXD/TXD/GND/3V3/GND/5V/LINK`. RXD/TXD (rechts) sind
dieselben UART0-Signale wie TXO/RXO (links), nur zweimal herausgefuehrt.

**Wichtige Korrektur gegenueber einer ungeprueften generischen Pinout-Notiz**
(`sensormeter/ESP32-ETH01 v1.4 pinout.txt`, dort steht faelschlich "CFG =
meist GPIO0"): laut bereits abgeschlossener Recherche im sensormeter-Projekt
(siehe dortiges `docs/entscheidungen.md` "IO32/IO33-Frage endgueltig geklaert",
Datenblatt + Foto + 3 Quellen) ist `CFG` in Wirklichkeit GPIO32 und `485_EN`
GPIO33 - normale GPIOs fuer eine optionale RS485-Zusatzbeschaltung, KEINE
Boot-Strap-Pins. Der tatsaechliche ESP32-Boot-Modus-Pin ist der separat
beschriftete `IO0`-Pin (linke Spalte). Flash-Anleitung verweist explizit
darauf, `CFG`/`485_EN` NICHT mit dem Boot-Modus zu verwechseln.

Flasher-Pins: `GND`, `TXO`↔Adapter-RXD, `RXO`↔Adapter-TXD, zusaetzlich `IO0`
+ `EN` fuer den manuellen Boot-Modus (Adapter ohne DTR/RTS) - Ablauf 1:1 aus
`sensormeter/docs/flash-bereitschaft.html` uebernommen (dort bereits an
echter Hardware erprobt, selbes Boardmodell). Spaetere USB-Stromversorgung im
Normalbetrieb: `5V` + das direkt darueberliegende `GND` (rechte Spalte) -
Board laeuft mit 5V ODER 3,3V, nie beides gleichzeitig.

`scripts/flash.ps1` ist eine auf ein einzelnes Projekt verschlankte Fassung
von `sensormeter/repo/scripts/flash.ps1` (kein Mehrprojekt-Auswahlmenue, da
ESP-Anwesenheit kein Geschwisterprojekt hat) - identische Toolchain-
Erkennung (funktionaler `--version`-Test statt reiner PATH-Pruefung, wegen
des Windows-eigenen "python"-Store-Alias-Fallstricks). Verifiziert:
PowerShell-Parser-Syntaxpruefung sauber, UND tatsaechlich mit `-SkipUpload`
lokal ausgefuehrt (nicht nur gelesen) - erkennt vorhandene Python/Git/
PlatformIO-Installation korrekt, findet firmware/platformio.ini relativ zu
seinem eigenen Pfad, laesst bestehende config.h unangetastet, Build lief
durch (Flash 80,3%). Der eigentliche Upload-Schritt (`pio run --target
upload`) ist NICHT verifiziert - kein Board angeschlossen.

## Projektstart (2026-07-29)

Grundgeruest aufgesetzt und erfolgreich gebaut (`pio run`, Flash 76%, RAM 28%
bei WT32-ETH01 / 4MB / min "default" Partitionsschema). Noch NICHT auf echter
Hardware getestet - siehe [[feedback_verify_and_document]]-Konvention: vor dem
naechsten Schritt (Windows-Client) sollte ein echter Flash-Test erfolgen.

## Direkte Uebernahme aus dem sensormeter-Projekt

Auf Nutzerwunsch 1:1 (mit Anpassungen) uebernommen, da dort bereits gehaertet:

- **NetworkManager**: Ethernet-Vorrang + optionales WLAN, eigener
  Fallback-Access-Point nach 5 Minuten ohne Verbindung, aktiver
  WLAN-Reconnect, LAN/WLAN-Interface-Pinning fuer NTP. Fallback-AP-Name
  bewusst geaendert (`anwesenheit-setup`/`anwesenheit` statt `installer`),
  damit beide Geraetefamilien im Netzwerk unterscheidbar bleiben.
- **TimeManager**: NTP-Sync mit LAN-vor-WLAN-Fehlerkette + DHCP-Test-Fallback,
  unveraendert.
- **OtaManager**: lokaler .bin-Upload inkl. des 2026-07-18 bei sensormeter
  behobenen Chunk-Groessen-Bugs im Marker-Scan. `FIRMWARE_PROJECT_ID
  "ESP-ANWESENHEIT"` verhindert Verwechslung mit anderen Projekten.
- **StorageManager**: unveraendert (LittleFS-Mount).
- **Auth-/Reset-/OTA-Handler-Muster** in WebServerManager (Basic-Auth fester
  Benutzername "admin", Werksreset mit Scope-Parameter, OTA-Upload-Callback-
  Reihenfolge).

**Nicht uebernommen:** RebootManager (taeglicher automatischer Neustart) -
nicht durch die Projektbeschreibung gefordert; die tageszeitgesteuerte aehnliche
Funktion (HistoryManager, taeglicher CSV-Flush) laeuft ueber eine eigene,
einfachere 24h-seit-Boot-Uhr statt einer festen Uhrzeit.

## ConfigManager: JSON statt XML

sensormeter nutzt config.xml + vendortes tinyxml2. Dieses Projekt speichert
stattdessen config.json ueber ArduinoJson (ohnehin lib_dep fuer die HTTP-API/
Event-Payloads) - kein zusaetzlicher XML-Parser noetig, ein Format weniger im
Projekt zu pflegen.

## Partitionstabelle: Standard statt min_spiffs.csv

sensormeter nutzt `min_spiffs.csv` (128 KB Datenpartition), weil dort das
OTA-Flash-Budget durch viele Adafruit-Sensor-Bibliotheken eng war. Dieses
Projekt hat deutlich weniger Abhaengigkeiten (nur ESPAsyncWebServer/AsyncTCP/
ArduinoJson), braucht dafuer aber mehr Platz fuer `logins.csv` (14 Tage
Ereignis-Historie). Die Standardpartitionierung gibt der Datenpartition
~1,31 MB statt 128 KB - reichlich Reserve fuer die CSV-Historie, waehrend das
tatsaechliche Firmware-Image (aktuell ~1 MB von 1,31 MB je OTA-Slot) trotzdem
noch in einen OTA-Slot passt.

## Event -> Status-Mapping

Die 7 in der Projektbeschreibung genannten Ereignistypen werden serverseitig
(EventManager::mapEventToState) auf einen von 4 Anzeigezustaenden abgebildet:

| event            | logontype | -> state    |
|------------------|-----------|-------------|
| login             | RDP       | RDP         |
| login             | Local/""  | Lokal       |
| unlock            | RDP       | RDP         |
| unlock            | Local/""  | Lokal       |
| lock              | -         | Gesperrt    |
| logout            | -         | Loginmaske (Benutzer geleert) |
| rdp-disconnect    | -         | Lokal       |
| switch-to-rdp     | -         | RDP         |

`rdp-disconnect` faellt auf "Lokal" zurueck, da die Windows-Ereignisse nur das
Trennen der RDP-Sitzung melden, nicht ob der Rechner danach tatsaechlich lokal
weiterbenutzt wird oder an der Loginmaske steht - ohne ein drittes
Zwischenzustandssignal ist "Lokal" die naheliegendste Annahme. Ein unbekannter
`event`-Wert wird als ungueltige Anfrage (HTTP 400) abgelehnt, nicht stillschweigend
ignoriert.

Client-Uhrzeit (`timestamp` im Payload) wird NICHT fuer state/lastUpdate
verwendet, nur als Rohwert mitgefuehrt (Anzeige/Audit in logins.csv) - viele
Windows-PCs im selben Netz koennen leicht unterschiedliche Systemuhren haben,
die Server-Empfangszeit ist die verlaesslichere Quelle fuer die Live-Anzeige.

## 500er-Ringpuffer vs. taeglicher CSV-Flush

Die Projektbeschreibung trennt bewusst zwei Dinge: einen RAM-Ringpuffer mit
"z.B. 500 Ereignissen" fuer die Live-Ansicht, und eine alle 24h daraus
verdichtete `logins.csv` mit 14 Tage Retention. Umgesetzt als: HistoryManager
merkt sich eine Sequenznummer-Wassermarke und uebernimmt bei jedem 24h-Zyklus
alle seit der letzten Uebernahme neuen Ereignisse (nicht nur den aktuellen
Ringpufferinhalt) - das funktioniert verlustfrei, SOLANGE zwischen zwei
Flushes nicht mehr als 500 Ereignisse anfallen (sonst werden aeltere durch den
Ringpuffer bereits ueberschrieben, bevor sie geflusht wurden). Bei ungewoehnlich
hohem Ereignisaufkommen waere entweder ein haeufigerer Flush oder ein groesserer
Ringpuffer noetig - fuer den erwarteten Einsatzzweck (Login-Ereignisse eines
Buero-Netzwerks) ist 500/Tag grosszuegig bemessen.

Nach einem Neustart beginnt die taegliche Historie bewusst neu (so explizit in
der Projektbeschreibung gefordert): Ringpuffer UND Wassermarke liegen nur im
RAM, ein Neustart vor dem naechsten 24h-Flush verliert die seither
aufgelaufenen Ereignisse. Das ist eine akzeptierte Einschraenkung, kein Bug.

## SNMP + Zabbix-Template (2026-07-29)

SNMP v1/v2c read-only Agent umgesetzt (`SNMPManager.h/.cpp`), Muster 1:1 aus
`sensormeter/repo/firmware/src/SNMPManager.cpp` uebernommen (`0neblock/
SNMP_Agent`-Bibliothek, `WiFiUDP`, periodischer `refreshValues()`-Refresh
alle 5s statt bei jedem GET, read-only durch Konstruktion erzwungen - nirgends
`isSettable=true`). `pio run` nach dem Einbau erneut gruen (Flash 80,3%,
RAM 28,9%).

OID-Schema exakt wie vom Nutzer vorgegeben, unter der in der
Sensormeter-Familie bereits verwendeten (frei erfundenen, unregistrierten)
Enterprise-Nummer `1.3.6.1.4.1.99999`:

| OID | Bedeutung | Typ |
|---|---|---|
| `.1.99999.1.1.0` | Systemname | String |
| `.1.99999.1.2.0` | Firmwareversion | String |
| `.1.99999.1.3.0` | Systemtyp ("ESP-Anwesenheit", statisch) | String |
| `.1.99999.2.1.0` | LAN-IP | String |
| `.1.99999.2.2.0` | WLAN-IP | String |
| `.1.99999.2.3.0` | WLAN-Signalstaerke | Integer, dBm |
| `.1.99999.2.4.0` | WLAN-SSID | String |
| `.1.99999.3.1.0` | Angemeldete Benutzer | **Gauge32** |
| `.1.99999.5.1.0` | Uptime | TimeTicks (Zentisekunden) |
| `.1.99999.5.2.0` | Freier Heap | Gauge32, Bytes |

Branches `.1`/`.2`/`.5` (System/Netzwerk/Status) tragen dieselbe Bedeutung
wie bei sensormeter - reine Familienkonvention, kein technischer Zwang.
Branch `.3` ist bei sensormeter "Sensor 1" (dort ohne Aequivalent hier);
fuer dieses Projekt neu belegt mit der eigentlichen Kernmetrik
("angemeldete Benutzer") statt sie irgendwo anzuhaengen - konsistent mit
ESP-BMCs Vorgehen, sich innerhalb der gemeinsamen Enterprise-Nummer einen
projekteigenen, bisher unbenutzten Branch zu nehmen.

**"Counter" -> Gauge32-Korrektur (Nutzerklaerung eingeholt)**: urspruenglich
als "Counter" bezeichnet, aber ein SNMP `Counter32` darf laut Standard nur
monoton steigen - ein Wert, der bei jeder Abmeldung wieder faellt, verletzt
das. Nutzer hat bestaetigt: gemeint ist die AKTUELLE Anzahl angemeldeter
Rechner (steigt und faellt), also `Gauge32`. Zaehlregel
(`EventManager::getLoggedInCount()`): state `!= "Loginmaske"` zaehlt als
angemeldet - ein gesperrter Bildschirm hat weiterhin eine aktive
Benutzersitzung, nur die Loginmaske bedeutet "niemand angemeldet".

`docs/zabbix-template-esp-anwesenheit.yaml` (Zabbix 6.4 YAML-Export, exakt im
Format/Stil der bestehenden Sensormeter-Templates) mit 10 Items + 3 Triggern
(keine SNMP-Daten seit 10min, freier Heap niedrig, optional zu viele
angemeldete Benutzer via `{$LOGGEDIN_MAX}`) + Makros `{$SNMP_COMMUNITY}`/
`{$HEAP_MIN_BYTES}`/`{$LOGGEDIN_MAX}`. Eigene Template-Gruppe "IoT
Anwesenheit" (nicht sensormeters "IoT Sensoren" - andere Produktlinie).
Technischer Name bewusst OHNE Klammern (`ESP-Anwesenheit WT32-ETH01`), nur
der Anzeigename traegt sie (`ESP-Anwesenheit (WT32-ETH01)`) - vermeidet den
bei sensormeter/sensormeter-display bereits aufgetretenen Zabbix-Import-
Fehler ("ungueltiger Hostname" durch Klammern im technischen Feld). YAML
strukturell mit PyYAML geparst und alle 10 OIDs 1:1 gegen SNMPManager.cpp
abgeglichen (Skript, kein echter Zabbix-Import) - ein echter Import in eine
laufende Zabbix-Instanz steht noch aus (in diesem Environment nicht
verfuegbar, wie bereits bei den anderen Familien-Templates dokumentiert).

## Nicht umgesetzt (bewusst zurueckgestellt)

- **Echte Offline-Erkennung per Heartbeat** (Bonusziel) - der Client sendet
  bisher nur ereignisgetrieben, kein periodischer Heartbeat. Die Web-UI dimmt
  Eintraege bereits, deren `lastUpdate` > 15 Minuten zurueckliegt (rein
  clientseitig aus dem ohnehin vorhandenen Zeitstempel berechnet) - eine
  echte Erkennung "PC ist ausgeschaltet" braucht aber einen eigenen
  Heartbeat-Mechanismus im Windows-Client, der noch nicht existiert.
- **Ping-basierte IP-Kollisionspruefung** vor dem Uebernehmen einer neuen
  statischen IP (wie bei sensormeter, ueber ESP32Ping) - hier bewusst
  weggelassen, um eine zusaetzliche Abhaengigkeit zu sparen; stattdessen nur
  Format-Validierung vor dem Uebernehmen.

## Windows-Client (2026-07-29)

`client/windows/AnwesenheitAgent.ps1` nutzt `Microsoft.Win32.SystemEvents.SessionSwitch`
(.NET, ueber `[Microsoft.Win32.SystemEvents]::add_SessionSwitch({...})`) statt
Security-Event-Log-Auswertung (Event-IDs 4624/4634/4778/4779 etc.) - letzteres
haette auf jedem Ziel-PC eine geaenderte Audit-Policy vorausgesetzt
(Anmeldungen werden standardmaessig NICHT auditiert). `SessionSwitch` liefert
alle benoetigten Uebergaenge direkt als .NET-Enum (`SessionSwitchReason`):
SessionLogon/-Logoff/-Lock/-Unlock, RemoteConnect/RemoteDisconnect. Getestet
(2026-07-29) per PowerShell-Parser (Syntaxpruefung aller 3 Skripte) und per
lokalem Mock-`HttpListener`: das gesendete JSON (`computer/user/event/
logontype/timestamp`) und `Content-Type: application/json` passen exakt zu
`WebServerManager::handleApiEvent` in der Firmware. NICHT auf echter Windows-
Anmeldung/-Sperrung getestet (kein Zielrechner verfuegbar) - das steht noch aus.

`ConsoleConnect`/`ConsoleDisconnect` (lokaler Benutzerwechsel per "Benutzer
wechseln") werden bewusst NICHT gemeldet - kein passendes Event unter den 7 in
der Projektbeschreibung festgelegten Typen, wuerden vom Server ohnehin mit
HTTP 400 abgelehnt.

**Deployment**: Scheduled Task "Bei Anmeldung" (`New-ScheduledTaskTrigger
-AtLogOn` ohne `-User`, Principal `BUILTIN\Users`/`Limited`) statt Windows-
Dienst - laeuft dadurch automatisch in der jeweiligen Benutzersitzung (fuer
`TerminalServerSession`/`SystemEvents` relevant) ohne Admin-Rechte im
Regelbetrieb; nur die einmalige Installation (`Install-AnwesenheitAgent.ps1`)
braucht Admin. Serveradresse liegt in einer separaten
`anwesenheit-client.json` neben dem Skript, nicht hart codiert - vereinfacht
das Verteilen auf mehrere PCs (`Install-AnwesenheitAgent.ps1 -ServerUrl ...`
kopiert Skript + schreibt die Config in einem Schritt).

**Bekannte Einschraenkung, real bestaetigt und teilweise entschaerft
(2026-07-30):** Bei Abmeldung/Herunterfahren kann Windows den Scheduled-
Task-Prozess beenden, bevor der "logout"-HTTP-POST abgeschlossen ist. Real
beobachtet auf einem echten Geraet: `/events` zeigte ausschliesslich
`login`-Ereignisse (4 Stueck ueber ~1h), nie ein `logout`/`lock`/`unlock`
davor - der urspruengliche Ablauf (5s Timeout + 2s Pause + zweiter 5s-
Versuch, bis zu ~12s) passte damit erkennbar zu langsam in das kurze
Zeitfenster, das Windows vor dem Prozess-Kill gewaehrt. Fix:
`Send-AnwesenheitEvent` bekam neue optionale `-TimeoutSec`/-`MaxAttempts`-
Parameter; der `SessionLogoff`-Handler ruft jetzt mit `-TimeoutSec 2
-MaxAttempts 1` auf (kein Retry) - erhoeht die Chance, das Zeitfenster
noch zu treffen, auf Kosten der Netzwerk-Resilienz genau fuer diesen einen
zeitkritischen Fall. Alle anderen Ereignisse (login/lock/unlock/RDP)
behalten die bisherigen 5s/2-Versuche.

Real getestet: derselbe verkuerzte Aufruf (2s Timeout, kein Retry) gegen
das echte Geraet beantwortet in ~155ms - das eigentliche Zeitproblem liegt
also nicht an der Serverantwortzeit, sondern ausschliesslich am
Windows-seitigen Abmelde-/Herunterfahren-Zeitfenster. **Nicht garantiert
vollstaendig geloest** - ob Windows in JEDEM Fall genuegend Zeit fuer auch
nur 2 Sekunden gewaehrt, haengt von Systemkonfiguration/-last ab; weiterhin
unkritisch, da die Web-UI Eintraege ab 15 Minuten Inaktivitaet abblendet
und der naechste Login den Status ohnehin korrigiert.

## RDS-Mehrfachsitzungs-Unterstuetzung (2026-07-29)

Nutzerfrage: laeuft der Windows-Client auch auf einem Windows-RDS-Host
(Remote Desktop Session Host, mehrere gleichzeitige Sitzungen auf demselben
Rechner)? Antwort: der Scheduled-Task/`SessionSwitch`-Mechanismus selbst
funktioniert dort unveraendert (Task Scheduler startet "Bei Anmeldung" pro
Sitzung neu, `SystemEvents`/`WTSRegisterSessionNotification` ist intrinsisch
sitzungsgebunden) - ABER `$env:COMPUTERNAME` allein identifiziert auf einem
RDS-Host NICHT die einzelne Sitzung: alle gleichzeitig angemeldeten Kollegen
haetten denselben Rechnernamen, `EventManager::applyToStatus` haette dadurch
nur EINEN Status-Eintrag fuer den ganzen Host, jede neue Anmeldung wuerde die
vorherige einfach ueberschreiben - fuer den erklaerten Zweck ("wer sitzt
gerade wo") auf einem RDS-Host unbrauchbar.

Fix in `client/windows/AnwesenheitAgent.ps1` (`Get-ComputerIdentifier`):
Erkennung ueber `Win32_OperatingSystem.ProductType` (CIM, keine erhoehten
Rechte noetig) - `1` (Workstation, z.B. Win10/11) laesst das `computer`-Feld
unveraendert (dort ist ohnehin nur eine interaktive Sitzung gleichzeitig
moeglich, RDP uebernimmt die bestehende Konsolensitzung statt eine zweite zu
eroeffnen); `2`/`3` (Domain Controller/Server, u.a. RDS-Hosts) haengt
`$env:SESSIONNAME` an (z.B. `RDSHOST01 (RDP-Tcp#5)`), damit jede Sitzung eine
eigene Zeile bekommt. Bewusst NICHT ueber "laeuft gerade mehr als eine
Sitzung" entschieden - ProductType steht sofort beim Skriptstart fest, ohne
wiederholte Sitzungsabfrage, und aendert sich waehrend der Laufzeit nicht.

Verifiziert (lokal, kein RDS-Host verfuegbar): Syntaxpruefung sauber, UND die
Funktion isoliert mit echtem `ProductType` (Workstation, Ergebnis
unveraendert `SPS-FLASH`) sowie mit simuliertem `ProductType=3`
(Server/RDS, Ergebnis `SPS-FLASH (Console)`) tatsaechlich ausgefuehrt - nicht
nur gelesen. Ein echter Test mit mehreren gleichzeitigen RDP-Sitzungen auf
einem echten RDS-Host steht noch aus.

## Web-UI

AJAX-Polling (alle 5s, `fetch()`) statt Server-Sent Events - konsistent mit
dem bereits bewaehrten sensormeter-Muster, keine zusaetzliche
SSE-Infrastruktur noetig. `font-size: clamp(...)` sorgt fuer sichtbare
Skalierung mit der Bildschirmbreite (Projektbeschreibung: "Die Seite skaliert
mit der Aufloesung des Userclients"), Tabellen liegen in `.tablewrap`-Containern
mit horizontalem Scroll fuer schmale Bildschirme.
