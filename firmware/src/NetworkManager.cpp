#include "NetworkManager.h"

#include "pins.h"
#include <ETH.h>  // muss nach pins.h stehen: ETH.h definiert dieselben Makronamen nur als Fallback via #ifndef
#include <esp_netif.h>
#include "lwip/netif.h"
#include "lwip/tcpip.h"  // LOCK_TCPIP_CORE()/UNLOCK_TCPIP_CORE()

NetworkManager* NetworkManager::_instance = nullptr;

static const unsigned long NETWORK_CHECK_TIMEOUT_MS = 5UL * 60UL * 1000UL;
static const unsigned long WLAN_TEST_TIMEOUT_MS = 30UL * 1000UL;
static const unsigned long FALLBACK_RETRY_INTERVAL_MS = 30UL * 1000UL;
static const unsigned long RECONNECT_RETRY_INTERVAL_MS = 20UL * 1000UL;
// Eigener Fallback-AP-Name fuer dieses Projekt (bewusst nicht "installer" wie
// bei sensormeter, um beide Geraetefamilien im Netzwerk unterscheidbar zu
// halten) - siehe docs/entscheidungen.md.
static const char* FALLBACK_WLAN_SSID = "anwesenheit-setup";
static const char* FALLBACK_WLAN_PSK = "anwesenheit";

static const IPAddress FALLBACK_AP_IP(192, 168, 4, 1);
static const IPAddress FALLBACK_AP_SUBNET(255, 255, 255, 0);

NetworkManager::NetworkManager(DataManager& dataManager, ConfigManager& configManager)
    : _data(dataManager), _config(configManager) {
  _instance = this;
}

String NetworkManager::sanitizeHostname(const String& name) {
  String out;
  for (size_t i = 0; i < name.length(); i++) {
    char c = name[i];
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-') {
      out += c;
    } else if ((c == ' ' || c == '_') && out.length() > 0 && out[out.length() - 1] != '-') {
      out += '-';
    }
  }
  while (out.length() > 0 && out[out.length() - 1] == '-') out.remove(out.length() - 1);
  while (out.length() > 0 && out[0] == '-') out.remove(0, 1);
  if (out.isEmpty()) out = "esp-anwesenheit";
  return out;
}

void NetworkManager::onNetworkEvent(WiFiEvent_t event) {
  if (!_instance) return;

  switch (event) {
    case ARDUINO_EVENT_ETH_START: {
      Serial.println("[NET] Ethernet gestartet");
      String hostname = sanitizeHostname(_instance->_config.getConfig().systemName);
      ETH.setHostname(hostname.c_str());
      break;
    }
    case ARDUINO_EVENT_ETH_CONNECTED:
      Serial.println("[NET] Ethernet-Kabel verbunden");
      _instance->_ethConnected = true;
      break;
    case ARDUINO_EVENT_ETH_GOT_IP:
      Serial.print("[NET] LAN-IP erhalten: ");
      Serial.println(ETH.localIP());
      _instance->_ethGotIp = true;
      break;
    case ARDUINO_EVENT_ETH_DISCONNECTED:
      Serial.println("[NET] Ethernet-Kabel getrennt");
      _instance->_ethConnected = false;
      _instance->_ethGotIp = false;
      break;
    case ARDUINO_EVENT_ETH_STOP:
      Serial.println("[NET] Ethernet gestoppt");
      _instance->_ethConnected = false;
      _instance->_ethGotIp = false;
      break;

    case ARDUINO_EVENT_WIFI_STA_CONNECTED:
      Serial.println("[NET] WLAN verbunden");
      break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      Serial.print("[NET] WLAN-IP erhalten: ");
      Serial.println(WiFi.localIP());
      _instance->_wlanGotIp = true;
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      Serial.println("[NET] WLAN-Verbindung verloren");
      _instance->_wlanGotIp = false;
      break;
    case ARDUINO_EVENT_WIFI_AP_STACONNECTED:
      Serial.println("[NET] Client mit Fallback-Access-Point verbunden");
      break;
    case ARDUINO_EVENT_WIFI_AP_STADISCONNECTED:
      Serial.println("[NET] Client vom Fallback-Access-Point getrennt");
      break;

    default:
      break;
  }
}

void NetworkManager::startFallbackAp() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_MODE_AP);
  WiFi.softAPConfig(FALLBACK_AP_IP, FALLBACK_AP_IP, FALLBACK_AP_SUBNET);
  _apActive = WiFi.softAP(FALLBACK_WLAN_SSID, FALLBACK_WLAN_PSK);

  if (_apActive) {
    Serial.print("[NET] Fallback-Access-Point \"");
    Serial.print(FALLBACK_WLAN_SSID);
    Serial.print("\" gestartet, IP: ");
    Serial.println(WiFi.softAPIP());
  } else {
    Serial.println("[NET] Fallback-Access-Point konnte nicht gestartet werden");
  }
}

void NetworkManager::applyLanConfig() {
  const DeviceConfig& cfg = _config.getConfig();
  if (cfg.lanDhcp) return;  // Default nach ETH.begin() ist bereits DHCP

  IPAddress ip, mask, gateway;
  if (ip.fromString(cfg.lanIp) && mask.fromString(cfg.lanMask) && gateway.fromString(cfg.lanGateway)) {
    IPAddress dns;
    if (cfg.lanDns.length() == 0 || !dns.fromString(cfg.lanDns)) {
      dns = gateway;
    }
    ETH.config(ip, gateway, mask, dns);
    Serial.println("[NET] LAN: statische IP angewendet (DNS: " + dns.toString() + ")");
  } else {
    Serial.println("[NET] LAN: statische IP konfiguriert, aber ungueltig -> bleibe bei DHCP");
  }
}

void NetworkManager::applyWlanConfig() {
  const DeviceConfig& cfg = _config.getConfig();
  if (cfg.wlanDhcp) return;

  IPAddress ip, mask, gateway;
  if (ip.fromString(cfg.wlanIp) && mask.fromString(cfg.wlanMask) && gateway.fromString(cfg.wlanGateway)) {
    IPAddress dns;
    if (cfg.wlanDns.length() == 0 || !dns.fromString(cfg.wlanDns)) {
      dns = gateway;
    }
    WiFi.config(ip, gateway, mask, dns);
    Serial.println("[NET] WLAN: statische IP angewendet (DNS: " + dns.toString() + ")");
  } else {
    Serial.println("[NET] WLAN: statische IP konfiguriert, aber ungueltig -> bleibe bei DHCP");
  }
}

bool NetworkManager::hasStaticConfig() const {
  const DeviceConfig& cfg = _config.getConfig();
  return !cfg.lanDhcp || !cfg.wlanDhcp;
}

void NetworkManager::beginDhcpFallbackTest() {
  Serial.println("[NET] NTP nicht erreichbar -> DHCP-Test auf statisch konfigurierten Interfaces");
  const DeviceConfig& cfg = _config.getConfig();
  if (!cfg.lanDhcp) ETH.config(IPAddress(0, 0, 0, 0), IPAddress(0, 0, 0, 0), IPAddress(0, 0, 0, 0));
  if (!cfg.wlanDhcp) WiFi.config(IPAddress(0, 0, 0, 0), IPAddress(0, 0, 0, 0), IPAddress(0, 0, 0, 0));
}

void NetworkManager::restoreConfiguredAddresses() {
  Serial.println("[NET] DHCP-Test erfolglos -> gesetzte IP-Konfiguration wiederherstellen");
  applyLanConfig();
  applyWlanConfig();
}

struct netif* NetworkManager::pinDefaultInterface(const String& choice) {
  const char* ifkey = (choice == "wlan") ? "WIFI_STA_DEF" : "ETH_DEF";
  esp_netif_t* target = esp_netif_get_handle_from_ifkey(ifkey);
  if (!target) return nullptr;
  int index = esp_netif_get_netif_impl_index(target);
  if (index <= 0) return nullptr;
  LOCK_TCPIP_CORE();
  struct netif* targetLwipNetif = netif_get_by_index((u8_t)index);
  struct netif* previous = netif_default;
  if (targetLwipNetif) netif_set_default(targetLwipNetif);
  UNLOCK_TCPIP_CORE();
  return targetLwipNetif ? previous : nullptr;
}

void NetworkManager::restoreDefaultInterface(struct netif* previous) {
  if (!previous) return;
  LOCK_TCPIP_CORE();
  netif_set_default(previous);
  UNLOCK_TCPIP_CORE();
}

void NetworkManager::begin() {
  _data.setSystemState(SystemState::INIT);
  Serial.println("[NET] Init: Ethernet + ggf. WLAN");

  WiFi.onEvent(onNetworkEvent);

  ETH.begin(ETH_PHY_ADDR, ETH_PHY_POWER, ETH_PHY_MDC, ETH_PHY_MDIO, ETH_PHY_TYPE, ETH_CLK_MODE);
  applyLanConfig();

  DeviceConfig cfg = _config.getConfig();

  if (cfg.wlanPendingTest) {
    _networkCheckTimeoutMs = WLAN_TEST_TIMEOUT_MS;
    cfg.wlanPendingTest = false;
    _config.setConfig(cfg);
    Serial.println("[NET] Neu eingetragenes WLAN wird getestet (kurzer Timeout)");
  } else {
    _networkCheckTimeoutMs = NETWORK_CHECK_TIMEOUT_MS;
  }

  _wlanConfigured = cfg.wlanSsid.length() > 0;
  if (_wlanConfigured) {
    WiFi.mode(WIFI_MODE_STA);
    WiFi.setAutoReconnect(true);
    // WLAN-Powersave abschalten - stabilisiert die Verbindung deutlich
    // (siehe sensormeter-family/docs "WLAN-Stabilitaetsfixes").
    WiFi.setSleep(false);
    WiFi.begin(cfg.wlanSsid.c_str(), cfg.wlanPsk.c_str());
    applyWlanConfig();
    Serial.println("[NET] WLAN-Verbindungsversuch gestartet (konfiguriertes Netz)");
  }

  _data.setSystemState(SystemState::NETWORK_CHECK);
  _networkCheckStartedMillis = millis();
  _lastReconnectAttemptMillis = millis();
}

void NetworkManager::loop() {
  logInterfaceTransitions();

  SystemState state = _data.getSystemState();

  if (state == SystemState::NETWORK_CHECK) {
    if (networkOk()) {
      _data.setSystemState(SystemState::RUN_NORMAL);
    } else if (millis() - _networkCheckStartedMillis > _networkCheckTimeoutMs) {
      _data.pushLogEntry("Netzwerk: kein Link, starte eigenen Access-Point \"" + String(FALLBACK_WLAN_SSID) + "\"",
                          DataManager::SEVERITY_ERROR);
      startFallbackAp();
      _lastFallbackJoinAttemptMillis = millis();
      _data.setSystemState(SystemState::FALLBACK_MODE);
    } else if (_wlanConfigured &&
               millis() - _lastReconnectAttemptMillis > RECONNECT_RETRY_INTERVAL_MS) {
      Serial.println("[NET] WLAN weg - aktiver Reconnect-Versuch");
      WiFi.reconnect();
      _lastReconnectAttemptMillis = millis();
    }
  } else if (state == SystemState::FALLBACK_MODE) {
    if (networkOk()) {
      _data.setSystemState(SystemState::RUN_NORMAL);
    } else if (millis() - _lastFallbackJoinAttemptMillis > FALLBACK_RETRY_INTERVAL_MS) {
      startFallbackAp();
      _lastFallbackJoinAttemptMillis = millis();
    }
  } else if (state == SystemState::RUN_NORMAL && !networkOk()) {
    _data.pushLogEntry("Netzwerk: Verbindung verloren (LAN+WLAN down)", DataManager::SEVERITY_ERROR);
    _apActive = false;
    _networkCheckTimeoutMs = NETWORK_CHECK_TIMEOUT_MS;
    _data.setSystemState(SystemState::NETWORK_CHECK);
    _networkCheckStartedMillis = millis();
    _lastReconnectAttemptMillis = millis();
  }
}

void NetworkManager::logInterfaceTransitions() {
  bool lanUp = _ethGotIp;
  if (_lanEverUp) {
    if (!lanUp && _lanDownSinceMillis == 0) {
      _lanDownSinceMillis = millis();
      _data.pushLogEntry("Netzwerk: LAN-Verbindung verloren", DataManager::SEVERITY_WARNING);
    } else if (lanUp && _lanDownSinceMillis != 0) {
      unsigned long downSec = (millis() - _lanDownSinceMillis) / 1000UL;
      _data.pushLogEntry("Netzwerk: LAN wieder verbunden (Ausfall " + String(downSec) + "s)",
                          DataManager::SEVERITY_INFO);
      _lanDownSinceMillis = 0;
    }
  }
  if (lanUp) _lanEverUp = true;

  bool wlanUp = _wlanGotIp;
  if (_wlanEverUp) {
    if (!wlanUp && _wlanDownSinceMillis == 0) {
      _wlanDownSinceMillis = millis();
      _data.pushLogEntry("Netzwerk: WLAN-Verbindung verloren", DataManager::SEVERITY_WARNING);
    } else if (wlanUp && _wlanDownSinceMillis != 0) {
      unsigned long downSec = (millis() - _wlanDownSinceMillis) / 1000UL;
      _data.pushLogEntry("Netzwerk: WLAN wieder verbunden (Ausfall " + String(downSec) + "s)",
                          DataManager::SEVERITY_INFO);
      _wlanDownSinceMillis = 0;
    }
  }
  if (wlanUp) _wlanEverUp = true;
}

IPAddress NetworkManager::getLanIp() const {
  return _ethGotIp ? ETH.localIP() : IPAddress(0, 0, 0, 0);
}

IPAddress NetworkManager::getWlanIp() const {
  if (_apActive) return WiFi.softAPIP();
  return _wlanGotIp ? WiFi.localIP() : IPAddress(0, 0, 0, 0);
}

IPAddress NetworkManager::getLanGateway() const {
  return _ethGotIp ? ETH.gatewayIP() : IPAddress(0, 0, 0, 0);
}

IPAddress NetworkManager::getWlanGateway() const {
  if (_apActive) return WiFi.softAPIP();
  return _wlanGotIp ? WiFi.gatewayIP() : IPAddress(0, 0, 0, 0);
}

IPAddress NetworkManager::getLanDns() const {
  return _ethGotIp ? ETH.dnsIP() : IPAddress(0, 0, 0, 0);
}

IPAddress NetworkManager::getWlanDns() const {
  if (_apActive) return IPAddress(0, 0, 0, 0);
  return _wlanGotIp ? WiFi.dnsIP() : IPAddress(0, 0, 0, 0);
}

String NetworkManager::getLanMac() const {
  return ETH.macAddress();
}

String NetworkManager::getWlanMac() const {
  return WiFi.macAddress();
}

String NetworkManager::getWlanSsid() const {
  if (_apActive) return String(FALLBACK_WLAN_SSID);
  return _wlanGotIp ? WiFi.SSID() : String("");
}

int NetworkManager::getWlanRssi() const {
  return _wlanGotIp ? WiFi.RSSI() : 0;
}
