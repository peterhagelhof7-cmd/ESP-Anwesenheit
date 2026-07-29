# Entscheidungen - ESP-Anwesenheit

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

**Bekannte Einschraenkung**: Bei Abmeldung kann der Scheduled Task beendet
werden, bevor der "logout"-HTTP-POST (bis zu 5s Timeout + 1 Retry)
abgeschlossen ist - ein gelegentlich fehlendes Logout-Ereignis ist deshalb
moeglich. Unkritisch, da die Web-UI Eintraege ab 15 Minuten Inaktivitaet
ohnehin abblendet (siehe oben) und der naechste Login den Status wieder
korrigiert.

## Web-UI

AJAX-Polling (alle 5s, `fetch()`) statt Server-Sent Events - konsistent mit
dem bereits bewaehrten sensormeter-Muster, keine zusaetzliche
SSE-Infrastruktur noetig. `font-size: clamp(...)` sorgt fuer sichtbare
Skalierung mit der Bildschirmbreite (Projektbeschreibung: "Die Seite skaliert
mit der Aufloesung des Userclients"), Tabellen liegen in `.tablewrap`-Containern
mit horizontalem Scroll fuer schmale Bildschirme.
