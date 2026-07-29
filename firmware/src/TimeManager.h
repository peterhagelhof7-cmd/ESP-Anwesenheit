#pragma once

#include <Arduino.h>
#include "DataManager.h"
#include "NetworkManager.h"

// NTP-Sync, 1:1 aus dem sensormeter-Projekt uebernommen: de.pool.ntp.org,
// 60s nach Boot, danach alle 5h, zusaetzlich sofort nach jedem Link-Up-Event.
// Sommerzeit (CET/CEST) per POSIX-TZ-String.
//
// Interface-Reihenfolge: ein NTP-Versuch wird zuerst explizit ueber LAN
// unternommen (NetworkManager::pinDefaultInterface). Schlaegt das 5 Minuten
// lang fehl UND ist WLAN verfuegbar, wird als naechstes 5 Minuten lang
// explizit ueber WLAN versucht (siehe sensormeter/docs/entscheidungen.md fuer
// die urspruengliche Fehleranalyse, die zu dieser Kette gefuehrt hat).
//
// Fehlerkette bei anhaltendem NTP-Ausfall: nur falls LAN oder WLAN statisch
// konfiguriert ist -> DHCP-Test, nach weiteren 3 Minuten ohne Erfolg ->
// Konfiguration wiederherstellen (ERROR_MODE).

class TimeManager {
 public:
  TimeManager(DataManager& dataManager, NetworkManager& networkManager);

  void begin();
  void loop();

  bool isSynced() const { return _synced; }

 private:
  enum class SyncPhase { Lan, Wlan };

  DataManager& _data;
  NetworkManager& _network;

  bool _synced = false;
  bool _wasNetworkUp = false;

  bool _attemptActive = false;
  unsigned long _attemptStartedMillis = 0;
  unsigned long _nextAttemptDueMillis = 0;
  SyncPhase _currentPhase = SyncPhase::Lan;
  struct netif* _pinnedPreviousNetif = nullptr;

  bool _dhcpTestActive = false;
  unsigned long _dhcpTestStartedMillis = 0;

  void startSyncAttempt();
  void onSyncSuccess();
  void unpinInterface();
};
