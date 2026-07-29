#include "ConfigManager.h"

#include <ArduinoJson.h>
#include <LittleFS.h>

namespace {
constexpr const char* kConfigFile = "/config.json";
constexpr const char* kConfigFileTmp = "/config.json.tmp";
}  // namespace

void ConfigManager::begin() {
  if (!load()) {
    Serial.println("[CONFIG] config.json fehlt/ungueltig -> Standardwerte werden gespeichert");
    _config = DeviceConfig();
    save();
  }
}

bool ConfigManager::load() {
  fs::File f = LittleFS.open(kConfigFile, "r");
  if (!f) return false;

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) {
    Serial.printf("[CONFIG] JSON-Parse-Fehler: %s\n", err.c_str());
    return false;
  }

  DeviceConfig cfg;
  cfg.systemName = doc["systemName"] | cfg.systemName;
  cfg.settingsPassword = doc["settingsPassword"] | cfg.settingsPassword;
  cfg.snmpCommunity = doc["snmpCommunity"] | cfg.snmpCommunity;

  JsonObject lan = doc["lan"];
  if (lan) {
    cfg.lanDhcp = lan["dhcp"] | cfg.lanDhcp;
    cfg.lanIp = lan["ip"] | cfg.lanIp;
    cfg.lanMask = lan["mask"] | cfg.lanMask;
    cfg.lanGateway = lan["gateway"] | cfg.lanGateway;
    cfg.lanDns = lan["dns"] | cfg.lanDns;
  }

  JsonObject wlan = doc["wlan"];
  if (wlan) {
    cfg.wlanDhcp = wlan["dhcp"] | cfg.wlanDhcp;
    cfg.wlanIp = wlan["ip"] | cfg.wlanIp;
    cfg.wlanMask = wlan["mask"] | cfg.wlanMask;
    cfg.wlanGateway = wlan["gateway"] | cfg.wlanGateway;
    cfg.wlanDns = wlan["dns"] | cfg.wlanDns;
    cfg.wlanSsid = wlan["ssid"] | cfg.wlanSsid;
    cfg.wlanPsk = wlan["psk"] | cfg.wlanPsk;
    cfg.wlanPendingTest = wlan["pendingTest"] | cfg.wlanPendingTest;
  }

  _config = cfg;
  return true;
}

bool ConfigManager::save() {
  JsonDocument doc;
  doc["systemName"] = _config.systemName;
  doc["settingsPassword"] = _config.settingsPassword;
  doc["snmpCommunity"] = _config.snmpCommunity;

  JsonObject lan = doc["lan"].to<JsonObject>();
  lan["dhcp"] = _config.lanDhcp;
  lan["ip"] = _config.lanIp;
  lan["mask"] = _config.lanMask;
  lan["gateway"] = _config.lanGateway;
  lan["dns"] = _config.lanDns;

  JsonObject wlan = doc["wlan"].to<JsonObject>();
  wlan["dhcp"] = _config.wlanDhcp;
  wlan["ip"] = _config.wlanIp;
  wlan["mask"] = _config.wlanMask;
  wlan["gateway"] = _config.wlanGateway;
  wlan["dns"] = _config.wlanDns;
  wlan["ssid"] = _config.wlanSsid;
  wlan["psk"] = _config.wlanPsk;
  wlan["pendingTest"] = _config.wlanPendingTest;

  // Erst in eine tmp-Datei schreiben und dann atomar umbenennen - ein
  // Stromausfall waehrend des Schreibens darf niemals eine halbfertige/leere
  // config.json hinterlassen (im Unterschied zum Log, siehe DataManager, wo
  // ein Verlust der letzten Zeilen unkritisch waere).
  fs::File f = LittleFS.open(kConfigFileTmp, "w");
  if (!f) {
    Serial.println("[CONFIG] config.json.tmp konnte nicht geschrieben werden");
    return false;
  }
  serializeJson(doc, f);
  f.close();

  LittleFS.remove(kConfigFile);
  return LittleFS.rename(kConfigFileTmp, kConfigFile);
}

void ConfigManager::setConfig(const DeviceConfig& config) {
  _config = config;
  save();
}
