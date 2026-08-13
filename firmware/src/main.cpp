// ============================================================================
// ESP-Anwesenheit (WT32-ETH01) - Windows Login Monitor
//
// Verdrahtet alle Module: ConfigManager laedt/speichert config.json auf
// LittleFS; NetworkManager bringt Ethernet (DHCP/statisch) und optional WLAN
// hoch und treibt den Boot-Zustandsautomaten an (eigener Fallback-Access-
// Point "anwesenheit-setup" nach 5 Minuten ohne Verbindung); TimeManager
// haengt sich mit der NTP-Sync-/DHCP-Testfehlerkette daran; EventManager
// nimmt die per POST /event eingehenden Windows-Anmeldeereignisse entgegen
// und haelt daraus den Live-Status je Rechner + einen 500er-Ringpuffer (RAM-
// only); HistoryManager verdichtet diesen Ringpuffer alle 24h in eine
// persistente, 14 Tage haltende logins.csv; WebServerManager stellt
// Hauptseite (Live-Status/Ereignisse per AJAX-Polling), Einstellungsseite,
// REST-API und lokalen OTA-Upload bereit (async, Port 80). NetworkManager/
// TimeManager/OtaManager/StorageManager sind (mit projektspezifischen
// Anpassungen) aus dem sensormeter-Projekt uebernommen - siehe
// docs/entscheidungen.md.
// ============================================================================

#include <Arduino.h>
#include <ESPmDNS.h>
#include <LittleFS.h>
#include <WiFiUdp.h>
#include <esp_task_wdt.h>

#include "ConfigManager.h"
#include "DataManager.h"
#include "EventManager.h"
#include "HistoryManager.h"
#include "NetworkManager.h"
#include "OtaManager.h"
#include "SNMPManager.h"
#include "StorageManager.h"
#include "SystemState.h"
#include "TimeManager.h"
#include "WebServerManager.h"

#if __has_include("config.h")
#include "config.h"
#else
#error "config.h fehlt! Kopiere include/config.h.example nach include/config.h."
#endif

// Eingebetteter Marker fuer die OTA-Herkunfts-/Versionspruefung (siehe
// OtaManager.h/.cpp) - referenziert via Serial.println() unten, damit der
// Linker ihn nicht wegoptimiert.
const char kFirmwareIdentityMarker[] = "SM-FW-ID:" FIRMWARE_PROJECT_ID ":" DEVICE_FIRMWARE_VERSION ":SM-FW-END";

// --- UDP-Auto-Discovery: Protokoll-Konstanten --------------------------------
// Der Windows-Client kann den ESP ohne fest eingetragene IP finden: er sendet
// einen UDP-Broadcast mit kDiscoveryRequest an kDiscoveryPort; dieses Geraet
// antwortet dem Absender per Unicast mit einem kleinen JSON (IP/Port/Endpunkt/
// Hostname/Version). Ergaenzt die schon vorhandene mDNS-Namensaufloesung fuer
// Netze, in denen mDNS/Multicast (UDP 5353) gefiltert ist, und macht den Client
// robust gegen DHCP-IP-Wechsel (er kann bei Sendefehlern neu suchen). Port +
// Anfrage-Kennung muessen mit dem Client (AnwesenheitAgent.ps1, Invoke-
// EspDiscovery) uebereinstimmen. Die Antwortfunktion handleDiscovery() steht
// unten bei den Managern (sie greift auf networkManager/configManager zu).
static const uint16_t kDiscoveryPort = 55321;
static const char kDiscoveryRequest[] = "ESP-ANWESENHEIT-DISCOVERY?";
static WiFiUDP discoveryUdp;
static bool discoveryStarted = false;

// Arduino-ESP32-Standardstack fuer loopTask ist 8192 Byte - im sensormeter-
// Projekt fuehrte die dort deutlich groessere Zahl gleichzeitig laufender
// Manager (Sensorik/Display/MQTT/SNMP/Syslog) zu einem Stack-Overflow-Crash,
// weshalb dort auf 16 KB verdoppelt wurde. Dieses Projekt hat davon nur einen
// Bruchteil der Module - 8 KB Default reicht hier voraussichtlich, wird aber
// vorsorglich ebenfalls verdoppelt, da der reale Speicherbedarf ohne
// Hardware-Test schwer abzuschaetzen ist und der Heap-Overhead (8 KB) gering
// ist. Siehe docs/entscheidungen.md.
SET_LOOP_TASK_STACK_SIZE(16384);

DataManager dataManager;
ConfigManager configManager;
StorageManager storageManager;
NetworkManager networkManager(dataManager, configManager);
TimeManager timeManager(dataManager, networkManager);
EventManager eventManager;
HistoryManager historyManager(eventManager, dataManager);
OtaManager otaManager;
WebServerManager webServerManager(dataManager, configManager, networkManager, otaManager, eventManager,
                                   historyManager);
SNMPManager snmpManager(configManager, networkManager, eventManager);

// Beantwortet eingehende UDP-Discovery-Broadcasts (Protokoll-Konstanten oben).
// Wird aus loop() gepollt (analog zu mDNS/SNMP) - die Antwort ist ein einzelnes
// kleines Paket, kein Blockieren.
void handleDiscovery() {
  if (!discoveryStarted) {
    if (!(networkManager.isLanUp() || networkManager.isWlanUp())) return;
    if (discoveryUdp.begin(kDiscoveryPort)) {
      discoveryStarted = true;
      Serial.printf("[NET] UDP-Discovery aktiv (Port %u)\n", kDiscoveryPort);
    }
    return;  // erst ab dem naechsten Durchlauf lauschen
  }

  int size = discoveryUdp.parsePacket();
  if (size <= 0) return;

  char buf[64];
  int n = discoveryUdp.read(buf, sizeof(buf) - 1);
  if (n < 0) n = 0;
  buf[n] = '\0';
  // Nur auf die exakte Anfrage-Kennung reagieren, fremde Pakete ignorieren.
  if (strncmp(buf, kDiscoveryRequest, strlen(kDiscoveryRequest)) != 0) return;

  // Die zu meldende IP im SELBEN /24 wie der Anfragende waehlen: das Geraet kann
  // auf LAN und WLAN in unterschiedlichen Subnetzen haengen (real beobachtet:
  // LAN 192.168.77.x, WLAN 192.168.178.x) - eine Antwort mit der "falschen"
  // Interface-IP waere fuer den Client nicht erreichbar. Fallback: LAN-Vorrang.
  IPAddress remote = discoveryUdp.remoteIP();
  IPAddress lan = networkManager.getLanIp();
  IPAddress wlan = networkManager.getWlanIp();
  bool lanUp = networkManager.isLanUp();
  bool wlanUp = networkManager.isWlanUp();
  auto sameSubnet24 = [](const IPAddress& a, const IPAddress& b) {
    return a[0] == b[0] && a[1] == b[1] && a[2] == b[2];
  };
  IPAddress ip;
  if (lanUp && sameSubnet24(lan, remote))        ip = lan;
  else if (wlanUp && sameSubnet24(wlan, remote))  ip = wlan;
  else if (lanUp)                                 ip = lan;
  else                                            ip = wlan;
  String host = NetworkManager::sanitizeHostname(configManager.getConfig().systemName);
  char reply[220];
  int len = snprintf(reply, sizeof(reply),
      "{\"service\":\"esp-anwesenheit\",\"ip\":\"%u.%u.%u.%u\",\"port\":80,"
      "\"path\":\"/event\",\"hostname\":\"%s\",\"version\":\"%s\"}",
      ip[0], ip[1], ip[2], ip[3], host.c_str(), DEVICE_FIRMWARE_VERSION);
  if (len <= 0) return;

  discoveryUdp.beginPacket(discoveryUdp.remoteIP(), discoveryUdp.remotePort());
  discoveryUdp.write(reinterpret_cast<const uint8_t*>(reply), len);
  discoveryUdp.endPacket();
}

// Serial-Kommandozeile fuer den Fall, dass das Geraet nur per USB, aber nicht
// per Netzwerk erreichbar ist (identisches Vertrauensmodell wie beim
// sensormeter-Projekt: physischer USB-Zugriff = vertrauenswuerdig, kein
// Web-Passwort noetig).
//
// Kommandos (jeweils + Enter):
//   dhcp <lan|wlan>                     Interface auf DHCP umstellen, neu starten
//   ip <lan|wlan> <ip> <maske> <gateway> [dns]   statische IP setzen, neu starten
//   wifi <ssid> <passwort>              neue WLAN-Zugangsdaten setzen, neu starten
//   status                              aktuellen Zustand ausgeben, kein Neustart
//   reset [config|data|all]             Werksreset (Default: all), neu starten
void handleSerialCommands() {
  static String line;

  while (Serial.available()) {
    char c = static_cast<char>(Serial.read());
    if (c == '\r') continue;
    if (c != '\n') {
      line += c;
      continue;
    }
    line.trim();

    String cmd = line;
    String args;
    int sp = line.indexOf(' ');
    if (sp >= 0) {
      cmd = line.substring(0, sp);
      args = line.substring(sp + 1);
      args.trim();
    }

    if (cmd.equalsIgnoreCase("dhcp")) {
      String iface = args;
      iface.trim();
      if (!iface.equalsIgnoreCase("lan") && !iface.equalsIgnoreCase("wlan")) {
        Serial.println("[SERIAL] Nutzung: dhcp <lan|wlan>");
      } else {
        DeviceConfig cfg = configManager.getConfig();
        if (iface.equalsIgnoreCase("lan")) {
          cfg.lanDhcp = true;
          cfg.lanIp = cfg.lanMask = cfg.lanGateway = cfg.lanDns = "";
        } else {
          cfg.wlanDhcp = true;
          cfg.wlanIp = cfg.wlanMask = cfg.wlanGateway = cfg.wlanDns = "";
        }
        configManager.setConfig(cfg);
        Serial.println("[SERIAL] " + iface + " auf DHCP umgestellt, starte neu...");
        delay(300);
        ESP.restart();
      }

    } else if (cmd.equalsIgnoreCase("ip")) {
      String iface;
      String rest = args;
      int sp1 = rest.indexOf(' ');
      if (sp1 >= 0) {
        iface = rest.substring(0, sp1);
        rest = rest.substring(sp1 + 1);
        rest.trim();
      }
      String parts[4];
      int count = 0;
      while (rest.length() > 0 && count < 4) {
        int sp2 = rest.indexOf(' ');
        if (sp2 < 0) {
          parts[count++] = rest;
          rest = "";
        } else {
          parts[count++] = rest.substring(0, sp2);
          rest = rest.substring(sp2 + 1);
          rest.trim();
        }
      }
      IPAddress probe;
      if ((!iface.equalsIgnoreCase("lan") && !iface.equalsIgnoreCase("wlan")) || count < 3 ||
          !probe.fromString(parts[0]) || !probe.fromString(parts[1]) || !probe.fromString(parts[2])) {
        Serial.println("[SERIAL] Nutzung: ip <lan|wlan> <adresse> <maske> <gateway> [dns]");
      } else {
        DeviceConfig cfg = configManager.getConfig();
        if (iface.equalsIgnoreCase("lan")) {
          cfg.lanDhcp = false;
          cfg.lanIp = parts[0];
          cfg.lanMask = parts[1];
          cfg.lanGateway = parts[2];
          cfg.lanDns = (count >= 4) ? parts[3] : "";
        } else {
          cfg.wlanDhcp = false;
          cfg.wlanIp = parts[0];
          cfg.wlanMask = parts[1];
          cfg.wlanGateway = parts[2];
          cfg.wlanDns = (count >= 4) ? parts[3] : "";
        }
        configManager.setConfig(cfg);
        Serial.println("[SERIAL] Statische IP (" + iface + ") gesetzt, starte neu...");
        delay(300);
        ESP.restart();
      }

    } else if (cmd.equalsIgnoreCase("wifi")) {
      int sp3 = args.indexOf(' ');
      if (sp3 < 0 || args.substring(0, sp3).length() == 0) {
        Serial.println("[SERIAL] Nutzung: wifi <ssid> <passwort>");
      } else {
        DeviceConfig cfg = configManager.getConfig();
        cfg.wlanSsid = args.substring(0, sp3);
        cfg.wlanPsk = args.substring(sp3 + 1);
        cfg.wlanPsk.trim();
        cfg.wlanPendingTest = true;
        configManager.setConfig(cfg);
        Serial.println("[SERIAL] WLAN-Zugangsdaten gesetzt, starte neu...");
        delay(300);
        ESP.restart();
      }

    } else if (cmd.equalsIgnoreCase("status")) {
      DeviceConfig cfg = configManager.getConfig();
      Serial.println("[SERIAL] --- Status ---");
      Serial.print("Zustand: ");
      Serial.println(toString(dataManager.getSystemState()));
      Serial.print("LAN: ");
      Serial.println(networkManager.isLanUp() ? "verbunden" : (networkManager.isLanLinkUp() ? "Link ohne IP" : "kein Link"));
      Serial.print("LAN-IP: ");
      Serial.println(networkManager.getLanIp());
      Serial.print("LAN-Modus: ");
      Serial.println(cfg.lanDhcp ? "DHCP" : "statisch");
      Serial.print("WLAN: ");
      if (networkManager.isUsingFallbackWlan()) {
        Serial.println("Fallback-Access-Point");
      } else if (networkManager.isWlanUp()) {
        Serial.print("verbunden mit ");
        Serial.println(cfg.wlanSsid);
      } else {
        Serial.println("nicht verbunden");
      }
      Serial.print("WLAN-IP: ");
      Serial.println(networkManager.getWlanIp());
      Serial.print("Bekannte Rechner: ");
      Serial.println(eventManager.getStatusCount());
      Serial.print("Freier Heap: ");
      Serial.print(ESP.getFreeHeap() / 1024);
      Serial.println(" kB");
      Serial.print("Laufzeit: ");
      Serial.print((unsigned long)(esp_timer_get_time() / 1000000ULL));
      Serial.println(" s");
      Serial.println("[SERIAL] --- Ende Status ---");

    } else if (cmd.equalsIgnoreCase("reset")) {
      String scope = args.length() > 0 ? args : "all";
      if (scope.equalsIgnoreCase("config") || scope.equalsIgnoreCase("all")) {
        configManager.setConfig(DeviceConfig());
      }
      if (scope.equalsIgnoreCase("data") || scope.equalsIgnoreCase("all")) {
        eventManager.clearAll();
        historyManager.clearHistory();
      }
      Serial.println("[SERIAL] Werksreset (" + scope + "), starte neu...");
      delay(300);
      ESP.restart();
    }

    line = "";
  }
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.print("=== ESP-Anwesenheit ");
  Serial.print(DEVICE_FIRMWARE_VERSION);
  Serial.println(" ===");
  Serial.println(kFirmwareIdentityMarker);
  Serial.println("[SERIAL] Kommandos: dhcp <lan|wlan>, ip <lan|wlan> ..., wifi <ssid> <psk>, status, "
                  "reset [config|data|all] (+ Enter)");

  dataManager.begin();
  dataManager.setSystemState(SystemState::BOOT);

  storageManager.begin();
  configManager.begin();
  eventManager.begin();
  historyManager.begin();
  timeManager.begin();

  networkManager.begin();  // setzt Zustand auf INIT, dann NETWORK_CHECK
  webServerManager.begin();  // async - kein eigener loop()-Aufruf noetig
  snmpManager.begin();

  // Task-Watchdog-Timer (TWDT) - siehe sensormeter/docs/entscheidungen.md
  // "Task-Watchdog (TWDT)": ohne esp_task_wdt_init() laeuft der TWDT zwar
  // per ESP-IDF-Default weiter, aber ohne Panic-Reaktion. 10s statt der
  // ESP-BMC-Vorgabe von 5s: WLAN-Scan (Einstellungsseite) und OTA-Schreiben
  // koennen kurzzeitig laenger als 5s dauern, ohne dass das ein echter Hang
  // waere.
  esp_task_wdt_init(10, true);
  esp_task_wdt_add(NULL);

  // Loop-Task-Handle an den OtaManager geben: waehrend eines OTA-Uploads wird
  // genau dieser Task kurz aus dem Watchdog ausgetragen, sonst loest das
  // Blockieren durch Update.write() einen Panic-Reboot mitten im Upload aus
  // (siehe OtaManager.cpp). Muss NACH esp_task_wdt_add(NULL) stehen.
  otaManager.setMainLoopTaskHandle(xTaskGetCurrentTaskHandle());
}

void loop() {
  handleSerialCommands();
  otaManager.checkStalled();  // Watchdog nach abgebrochenem Upload wieder scharf
  networkManager.loop();
  timeManager.loop();
  historyManager.loop();
  snmpManager.loop();

  handleDiscovery();

  static bool mdnsStarted = false;
  if (!mdnsStarted && (networkManager.isLanUp() || networkManager.isWlanUp())) {
    String hostname = NetworkManager::sanitizeHostname(configManager.getConfig().systemName);
    if (MDNS.begin(hostname.c_str())) {
      MDNS.addService("http", "tcp", 80);
      Serial.printf("[NET] mDNS gestartet: http://%s.local/\n", hostname.c_str());
    } else {
      Serial.println("[NET] mDNS-Start fehlgeschlagen");
    }
    mdnsStarted = true;
  }

  // Alle 5 Minuten pruefen, ob veraltete Status-Zeilen entfernt werden
  // sollen (Einstellungsseite "staleEntryHours", Default 2h, 0 = aus).
  // 5 Minuten statt jeder Schleife: die Pruefung selbst ist zwar billig
  // (kleiner Vector), aber ein 5-Minuten-Takt reicht fuer eine
  // Stunden-Schwelle voellig und spart unnoetige Mutex-Locks.
  static unsigned long lastStalePruneMillis = 0;
  const unsigned long STALE_PRUNE_INTERVAL_MS = 5UL * 60UL * 1000UL;
  if (millis() - lastStalePruneMillis >= STALE_PRUNE_INTERVAL_MS) {
    lastStalePruneMillis = millis();
    uint16_t staleHours = configManager.getConfig().staleEntryHours;
    if (staleHours > 0) {
      size_t removed = eventManager.pruneStaleStatuses((unsigned long)staleHours * 3600UL);
      if (removed > 0) {
        dataManager.pushLogEntry("Status: " + String(removed) + " veraltete(r) Eintrag/Eintraege entfernt (>" +
                                  String(staleHours) + "h inaktiv)");
      }
    }
  }

  esp_task_wdt_reset();
  delay(50);
}
