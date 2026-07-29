#pragma once

#include <Arduino.h>
#include <freertos/semphr.h>
#include "SystemState.h"

// Zentrale Systemdatenhaltung: Boot-Zustand + Ereignisprotokoll. Thread-safe
// fuer Zugriff aus mehreren Tasks (Network/Web). Die eigentlichen
// Anwesenheits-Ereignisse (Login/Logout/...) liegen NICHT hier, sondern in
// EventManager - DataManager entspricht funktional dem gleichnamigen Modul
// aus dem sensormeter-Projekt, aber ohne dessen Sensor-Ringpuffer (hier nicht
// benoetigt).

struct LogEntry {
  time_t timestamp = 0;
  String message;
  int severity = 6;      // Syslog-Konvention: 3 = Error, 4 = Warning, 6 = Info
  unsigned long sequence = 0;
};

class DataManager {
 public:
  static const int SEVERITY_ERROR = 3;
  static const int SEVERITY_WARNING = 4;
  static const int SEVERITY_INFO = 6;

  static const size_t LOG_CAPACITY = 5;  // RAM-Ringpuffer fuer die Webseite
  // Persistenter Log-Puffer auf LittleFS (/log.txt, Rotation nach
  // /log.old.txt) - identisches Schema wie sensormeter, siehe dortige
  // docs/entscheidungen.md fuer die Groessenrechnung.
  static const size_t LOG_FILE_MAX_BYTES = 32UL * 1024UL;

  void begin();

  SystemState getSystemState();
  void setSystemState(SystemState state);

  void pushLogEntry(const String& message, int severity = SEVERITY_INFO);
  size_t getLogEntries(LogEntry* out, size_t maxCount);  // neueste zuerst

 private:
  void appendLogFile(time_t timestamp, int severity, const String& message);

  SemaphoreHandle_t _mutex = nullptr;

  SystemState _systemState = SystemState::BOOT;

  LogEntry _log[LOG_CAPACITY];
  size_t _logCount = 0;
  size_t _logNextIndex = 0;
  unsigned long _logSequenceCounter = 0;
};
