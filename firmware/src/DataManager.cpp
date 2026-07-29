#include "DataManager.h"

#include <LittleFS.h>

namespace {
constexpr const char* kLogFile = "/log.txt";
constexpr const char* kLogFileOld = "/log.old.txt";

const char* severityLabel(int severity) {
  if (severity <= DataManager::SEVERITY_ERROR) return "ERR ";
  if (severity <= DataManager::SEVERITY_WARNING) return "WARN";
  return "INFO";
}
}  // namespace

void DataManager::begin() {
  _mutex = xSemaphoreCreateMutex();
}

SystemState DataManager::getSystemState() {
  SystemState state;
  xSemaphoreTake(_mutex, portMAX_DELAY);
  state = _systemState;
  xSemaphoreGive(_mutex);
  return state;
}

void DataManager::setSystemState(SystemState state) {
  xSemaphoreTake(_mutex, portMAX_DELAY);
  bool changed = (_systemState != state);
  _systemState = state;
  xSemaphoreGive(_mutex);
  if (changed) {
    Serial.printf("[STATE] -> %s\n", toString(state));
  }
}

void DataManager::pushLogEntry(const String& message, int severity) {
  time_t ts = time(nullptr);
  xSemaphoreTake(_mutex, portMAX_DELAY);
  _logSequenceCounter++;
  _log[_logNextIndex].timestamp = ts;
  _log[_logNextIndex].message = message;
  _log[_logNextIndex].severity = severity;
  _log[_logNextIndex].sequence = _logSequenceCounter;
  _logNextIndex = (_logNextIndex + 1) % LOG_CAPACITY;
  if (_logCount < LOG_CAPACITY) _logCount++;
  xSemaphoreGive(_mutex);
  Serial.printf("[LOG] %s\n", message.c_str());
  appendLogFile(ts, severity, message);
}

void DataManager::appendLogFile(time_t timestamp, int severity, const String& message) {
  fs::File existing = LittleFS.open(kLogFile, "r");
  size_t currentSize = existing ? existing.size() : 0;
  if (existing) existing.close();
  if (currentSize >= LOG_FILE_MAX_BYTES) {
    LittleFS.remove(kLogFileOld);
    LittleFS.rename(kLogFile, kLogFileOld);
  }

  fs::File f = LittleFS.open(kLogFile, "a");
  if (!f) {
    Serial.println("[DATA] log.txt konnte nicht geoeffnet werden (Anhaengen)");
    return;
  }
  struct tm tmv;
  localtime_r(&timestamp, &tmv);
  char tsBuf[20];
  snprintf(tsBuf, sizeof(tsBuf), "%04d-%02d-%02d %02d:%02d:%02d", tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
           tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
  f.printf("%s [%s] %s\n", tsBuf, severityLabel(severity), message.c_str());
  f.close();
}

size_t DataManager::getLogEntries(LogEntry* out, size_t maxCount) {
  size_t count;
  xSemaphoreTake(_mutex, portMAX_DELAY);
  count = min(_logCount, maxCount);
  for (size_t i = 0; i < count; i++) {
    size_t index = (_logNextIndex + LOG_CAPACITY - 1 - i) % LOG_CAPACITY;
    out[i] = _log[index];
  }
  xSemaphoreGive(_mutex);
  return count;
}
