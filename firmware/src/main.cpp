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
}

void loop() {
  handleSerialCommands();
  networkManager.loop();
  timeManager.loop();
  historyManager.loop();
  snmpManager.loop();

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

  esp_task_wdt_reset();
  delay(50);
}
