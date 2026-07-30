#pragma once

#include <Arduino.h>

// Laufzeitkonfiguration, persistiert als config.json auf LittleFS. Anders als
// sensormeter (config.xml + tinyxml2) bewusst JSON: dieses Projekt sendet/
// empfaengt ohnehin JSON (Event-Payloads, /status, /events) und hat keine
// vendorte XML-Bibliothek noetig, wenn ArduinoJson (bereits lib_dep fuer die
// HTTP-API) die Konfiguration gleich mit erledigt.
//
// Schema (config.json):
// {
//   "systemName": "ESP-Anwesenheit",
//   "settingsPassword": "admin",
//   "snmpCommunity": "public",
//   "staleEntryHours": 2,
//   "lan": {"dhcp": true, "ip": "", "mask": "", "gateway": "", "dns": ""},
//   "wlan": {"dhcp": true, "ip": "", "mask": "", "gateway": "", "dns": "",
//             "ssid": "", "psk": "", "pendingTest": false}
// }

struct DeviceConfig {
  String systemName = "ESP-Anwesenheit";
  // Default lt. Lastenheft: Benutzername fest "admin", Passwort "admin".
  String settingsPassword = "admin";
  // SNMP-Community fuer den read-only v1/v2c-Agenten (siehe SNMPManager).
  String snmpCommunity = "public";
  // Entfernt Status-Zeilen (EventManager, /status), deren letzte Meldung
  // laenger als diese Anzahl Stunden zurueckliegt - betrifft ALLE Zustaende
  // (auch Lokal/RDP/Gesperrt), nicht nur die Loginmaske: ein Rechner, der
  // sich einfach nie wieder meldet (aus, Agent deinstalliert, dauerhafter
  // Netzwerkausfall), soll nicht fuer immer in der Uebersicht haengen
  // bleiben. 0 = Aufraeumen deaktiviert. Siehe EventManager::pruneStaleStatuses().
  uint16_t staleEntryHours = 2;

  bool lanDhcp = true;
  String lanIp;
  String lanMask;
  String lanGateway;
  String lanDns;  // leer = Gateway als DNS verwenden

  bool wlanDhcp = true;
  String wlanIp;
  String wlanMask;
  String wlanGateway;
  String wlanDns;  // leer = Gateway als DNS verwenden
  String wlanSsid;
  String wlanPsk;
  // Einmal-Flag: siehe NetworkManager::begin() - ueberlebt nur einen Neustart.
  bool wlanPendingTest = false;
};

class ConfigManager {
 public:
  // Laedt config.json von LittleFS. Fehlt die Datei oder ist sie ungueltig,
  // werden Defaults verwendet und sofort als neue config.json gespeichert.
  void begin();

  const DeviceConfig& getConfig() const { return _config; }

  // Uebernimmt eine neue Konfiguration und speichert sie sofort.
  void setConfig(const DeviceConfig& config);

  bool save();

 private:
  DeviceConfig _config;
  bool load();
};
