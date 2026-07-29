#pragma once

#include <Arduino.h>
#include <SNMP_Agent.h>
#include <WiFiUdp.h>
#include "ConfigManager.h"
#include "EventManager.h"
#include "NetworkManager.h"

// SNMP v1/v2c read-only Agent (Bonusziel, siehe Projektbeschreibung.txt) -
// Muster 1:1 aus dem sensormeter-Projekt uebernommen (Bibliothek antwortet in
// der Version der eingehenden Anfrage; "read-only" ist hier durch
// Konstruktion erzwungen - es wird nirgends isSettable=true gesetzt). Werte
// werden alle 5s aktualisiert statt bei jedem GET neu berechnet, identisch
// zu sensormeters "polling optimized, no continuous refresh".
//
// OID-Schema unter der (in der Sensormeter-Familie bereits frei erfundenen,
// unregistrierten) Enterprise-Nummer .1.3.6.1.4.1.99999 - Branches .1/.2/.5
// (System/Netzwerk/Status) folgen exakt der sensormeter-Bedeutung, Branch .3
// ist bei sensormeter "Sensor 1", hier stattdessen fuer die
// Anwesenheits-Kernmetrik (angemeldete Benutzer) neu belegt - siehe
// docs/entscheidungen.md.
class SNMPManager {
 public:
  SNMPManager(ConfigManager& configManager, NetworkManager& networkManager, EventManager& eventManager);

  void begin();
  void loop();

 private:
  ConfigManager& _config;
  NetworkManager& _network;
  EventManager& _events;

  WiFiUDP _udp;
  SNMPAgent _agent;

  unsigned long _lastRefreshMillis = 0;

  // Werden periodisch befuellt (refreshValues()) und der Bibliothek als
  // Zeiger uebergeben - sie liest bei jedem GET live von dieser Adresse.
  char _systemName[33] = {0};
  char _lanIp[16] = {0};
  char _wlanIp[16] = {0};
  char _wlanSsid[33] = {0};
  char* _systemNamePtr = _systemName;
  char* _lanIpPtr = _lanIp;
  char* _wlanIpPtr = _wlanIp;
  char* _wlanSsidPtr = _wlanSsid;

  int _wlanRssi = 0;
  uint32_t _uptimeTicks = 0;
  uint32_t _freeHeap = 0;
  uint32_t _loggedInCount = 0;

  void refreshValues();
};
