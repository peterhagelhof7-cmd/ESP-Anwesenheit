#include "SNMPManager.h"

#include <esp_timer.h>

// Basis-OID + Struktur siehe SNMPManager.h und docs/entscheidungen.md.
static const char* OID_SYSTEM_NAME = ".1.3.6.1.4.1.99999.1.1.0";
static const char* OID_FIRMWARE = ".1.3.6.1.4.1.99999.1.2.0";
static const char* OID_SYSTEM_TYPE = ".1.3.6.1.4.1.99999.1.3.0";

static const char* OID_LAN_IP = ".1.3.6.1.4.1.99999.2.1.0";
static const char* OID_WLAN_IP = ".1.3.6.1.4.1.99999.2.2.0";
static const char* OID_WLAN_RSSI = ".1.3.6.1.4.1.99999.2.3.0";
static const char* OID_WLAN_SSID = ".1.3.6.1.4.1.99999.2.4.0";

// Neue, projektspezifische Belegung von Branch .3 (bei sensormeter "Sensor
// 1", hier ohne Aequivalent) - Kernmetrik dieses Projekts: wie viele Rechner
// gerade eine aktive Benutzersitzung haben (Lokal/RDP/Gesperrt zaehlen dazu,
// nur "Loginmaske" nicht - siehe EventManager::getLoggedInCount()). Bewusst
// Gauge32 statt Counter32: der Wert steigt UND faellt, ein SNMP-Counter darf
// laut Standard nur monoton steigen.
static const char* OID_LOGGEDIN_COUNT = ".1.3.6.1.4.1.99999.3.1.0";

static const char* OID_UPTIME = ".1.3.6.1.4.1.99999.5.1.0";  // TimeTicks (1/100s)
static const char* OID_HEAP = ".1.3.6.1.4.1.99999.5.2.0";    // Bytes

#if __has_include("config.h")
#include "config.h"
#endif
#ifndef DEVICE_FIRMWARE_VERSION
#define DEVICE_FIRMWARE_VERSION "0.0.0"
#endif

static const unsigned long REFRESH_INTERVAL_MS = 5UL * 1000UL;

SNMPManager::SNMPManager(ConfigManager& configManager, NetworkManager& networkManager, EventManager& eventManager)
    : _config(configManager), _network(networkManager), _events(eventManager) {}

void SNMPManager::refreshValues() {
  const DeviceConfig& cfg = _config.getConfig();

  strncpy(_systemName, cfg.systemName.c_str(), sizeof(_systemName) - 1);
  strncpy(_lanIp, _network.isLanUp() ? _network.getLanIp().toString().c_str() : "0.0.0.0", sizeof(_lanIp) - 1);
  strncpy(_wlanIp, _network.isWlanUp() ? _network.getWlanIp().toString().c_str() : "0.0.0.0", sizeof(_wlanIp) - 1);
  strncpy(_wlanSsid, _network.isWlanUp() ? _network.getWlanSsid().c_str() : "", sizeof(_wlanSsid) - 1);

  _wlanRssi = _network.isWlanUp() ? _network.getWlanRssi() : 0;

  _loggedInCount = (uint32_t)_events.getLoggedInCount();

  _uptimeTicks = (uint32_t)(esp_timer_get_time() / 10000ULL);  // Zentisekunden
  _freeHeap = ESP.getFreeHeap();
}

void SNMPManager::begin() {
  const DeviceConfig& cfg = _config.getConfig();
  _agent.setReadOnlyCommunity(cfg.snmpCommunity.c_str());
  _agent.setReadWriteCommunity(cfg.snmpCommunity.c_str());
  _agent.setUDP(&_udp);

  refreshValues();

  _agent.addReadWriteStringHandler(OID_SYSTEM_NAME, &_systemNamePtr, sizeof(_systemName), false);
  _agent.addReadOnlyStaticStringHandler(OID_FIRMWARE, std::string(DEVICE_FIRMWARE_VERSION));
  _agent.addReadOnlyStaticStringHandler(OID_SYSTEM_TYPE, std::string("ESP-Anwesenheit"));

  _agent.addReadWriteStringHandler(OID_LAN_IP, &_lanIpPtr, sizeof(_lanIp), false);
  _agent.addReadWriteStringHandler(OID_WLAN_IP, &_wlanIpPtr, sizeof(_wlanIp), false);
  _agent.addIntegerHandler(OID_WLAN_RSSI, &_wlanRssi, false);
  _agent.addReadWriteStringHandler(OID_WLAN_SSID, &_wlanSsidPtr, sizeof(_wlanSsid), false);

  _agent.addGaugeHandler(OID_LOGGEDIN_COUNT, &_loggedInCount);

  _agent.addTimestampHandler(OID_UPTIME, &_uptimeTicks, false);
  _agent.addGaugeHandler(OID_HEAP, &_freeHeap);

  _agent.sortHandlers();
  _agent.begin();

  Serial.println("[SNMP] Agent gestartet (v1/v2c read-only, Port 161)");
}

void SNMPManager::loop() {
  _agent.loop();

  if (millis() - _lastRefreshMillis >= REFRESH_INTERVAL_MS) {
    _lastRefreshMillis = millis();
    refreshValues();
  }
}
