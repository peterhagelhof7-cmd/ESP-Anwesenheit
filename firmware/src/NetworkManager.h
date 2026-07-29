#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include "ConfigManager.h"
#include "DataManager.h"

// Vorwaertsdeklaration statt <lwip/netif.h> hier einzubinden.
struct netif;

// Treibt den Boot-Zustandsautomaten an: BOOT -> INIT -> NETWORK_CHECK ->
// RUN_NORMAL bzw. FALLBACK_MODE. 1:1 aus dem sensormeter-Projekt uebernommen
// (dort ausfuehrlich gehaertet: LAN-Vorrang, aktiver WLAN-Reconnect, eigener
// Fallback-Access-Point nach 5 Minuten ohne Verbindung) - siehe
// docs/entscheidungen.md fuer die Herkunft und was hier NICHT uebernommen
// wurde (Sensorik/Relais/Display-Anbindung, hier nicht vorhanden).
class NetworkManager {
 public:
  NetworkManager(DataManager& dataManager, ConfigManager& configManager);

  void begin();
  void loop();

  bool isLanUp() const { return _ethGotIp; }
  bool isLanLinkUp() const { return _ethConnected; }
  bool isWlanUp() const { return _wlanGotIp || _apActive; }
  bool isUsingFallbackWlan() const { return _apActive; }

  IPAddress getLanIp() const;
  IPAddress getWlanIp() const;
  IPAddress getLanGateway() const;
  IPAddress getWlanGateway() const;
  IPAddress getLanDns() const;
  IPAddress getWlanDns() const;
  String getLanMac() const;
  String getWlanMac() const;
  String getWlanSsid() const;
  int getWlanRssi() const;

  // Fuer TimeManager (5 min ohne NTP -> DHCP aktivieren, nach weiteren 3 min
  // -> gesetzte IP-Einstellungen wiederherstellen).
  bool hasStaticConfig() const;
  void beginDhcpFallbackTest();
  void restoreConfiguredAddresses();

  // Pinnt lwIP's globalen Default-Netif fest auf "lan" oder "wlan" - fuer
  // TimeManagers LAN-vor-WLAN-NTP-Fehlerkette. Aufrufer muessen nach ihrem
  // Versuch stets restoreDefaultInterface() aufrufen.
  static struct netif* pinDefaultInterface(const String& choice);
  static void restoreDefaultInterface(struct netif* previous);

  // Leitet aus dem frei eingebbaren Systemnamen einen DNS-/mDNS-tauglichen
  // Hostnamen ab (nur a-z/0-9/-).
  static String sanitizeHostname(const String& name);

 private:
  DataManager& _data;
  ConfigManager& _config;

  unsigned long _networkCheckStartedMillis = 0;
  unsigned long _lastFallbackJoinAttemptMillis = 0;
  unsigned long _lastReconnectAttemptMillis = 0;
  bool _wlanConfigured = false;
  unsigned long _networkCheckTimeoutMs = 0;

  bool _lanEverUp = false;
  bool _wlanEverUp = false;
  unsigned long _lanDownSinceMillis = 0;
  unsigned long _wlanDownSinceMillis = 0;
  void logInterfaceTransitions();

  void applyLanConfig();
  void applyWlanConfig();
  void startFallbackAp();
  bool networkOk() const { return _ethGotIp || _wlanGotIp || _apActive; }

  static NetworkManager* _instance;
  static void onNetworkEvent(WiFiEvent_t event);

  volatile bool _ethConnected = false;
  volatile bool _ethGotIp = false;
  volatile bool _wlanGotIp = false;
  volatile bool _apActive = false;
};
