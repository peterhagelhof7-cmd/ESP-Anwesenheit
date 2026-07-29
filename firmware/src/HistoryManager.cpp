#include "HistoryManager.h"

#include <LittleFS.h>
#include <vector>
#include "TimeUtils.h"

namespace {
constexpr const char* kCsvFile = "/logins.csv";
constexpr const char* kCsvFileTmp = "/logins.csv.tmp";
constexpr const char* kCsvHeader = "epoch,computer,user,event,logontype,client_timestamp";
}  // namespace

const char* HistoryManager::csvPath() { return kCsvFile; }

HistoryManager::HistoryManager(EventManager& eventManager, DataManager& dataManager)
    : _events(eventManager), _data(dataManager) {}

void HistoryManager::begin() {
  _lastFlushMillis = millis();
  _lastFlushedSequence = 0;
}

void HistoryManager::loop() {
  if (millis() - _lastFlushMillis < kFlushIntervalMs) return;
  _lastFlushMillis = millis();
  flushToCsv();
}

void HistoryManager::flushToCsv() {
  LoginEvent buf[EventManager::RINGBUFFER_SIZE];
  size_t count = _events.getEventsAfter(_lastFlushedSequence, buf, EventManager::RINGBUFFER_SIZE);
  if (count == 0) return;

  bool isNewFile = !LittleFS.exists(kCsvFile);
  fs::File f = LittleFS.open(kCsvFile, "a");
  if (!f) {
    Serial.println("[HISTORY] logins.csv konnte nicht geoeffnet werden (Anhaengen)");
    return;
  }
  if (isNewFile) {
    f.println(kCsvHeader);
  }
  for (size_t i = 0; i < count; i++) {
    const LoginEvent& e = buf[i];
    f.printf("%ld,%s,%s,%s,%s,%s\n", static_cast<long>(e.serverTime), e.computer.c_str(), e.user.c_str(),
              e.event.c_str(), e.logontype.c_str(), e.clientTimestamp.c_str());
    _lastFlushedSequence = e.sequence;
  }
  f.close();

  _data.pushLogEntry("Historie: " + String(count) + " Ereignis(se) in logins.csv uebernommen",
                      DataManager::SEVERITY_INFO);

  pruneOldRows();
}

// Loescht Zeilen aelter als kRetentionDays und deckelt die Dateigroesse als
// Sicherheitsnetz - siehe HistoryManager.h fuer die Begruendung, warum ohne
// synchronisierte Uhr NICHT geprueft wird (sonst koennte eine falsch
// gehende RTC kurz nach dem Boot versehentlich alles loeschen).
void HistoryManager::pruneOldRows() {
  if (!isTimeSynced()) return;

  fs::File in = LittleFS.open(kCsvFile, "r");
  if (!in) return;

  time_t cutoff = time(nullptr) - (static_cast<time_t>(kRetentionDays) * 86400L);

  std::vector<String> kept;
  size_t keptBytes = 0;
  bool first = true;
  while (in.available()) {
    String line = in.readStringUntil('\n');
    line.trim();
    if (line.isEmpty()) continue;
    if (first) {
      first = false;
      continue;  // Header wird unten neu geschrieben
    }
    int comma = line.indexOf(',');
    if (comma < 0) continue;
    time_t epoch = static_cast<time_t>(line.substring(0, comma).toInt());
    if (epoch < cutoff) continue;
    kept.push_back(line);
    keptBytes += line.length() + 1;
  }
  in.close();

  // Sicherheitsdeckel: aelteste (vorderste) verbleibende Zeilen verwerfen,
  // bis die Datei wieder unter kCsvMaxBytes liegt.
  size_t startIndex = 0;
  while (keptBytes > kCsvMaxBytes && startIndex < kept.size()) {
    keptBytes -= kept[startIndex].length() + 1;
    startIndex++;
  }

  fs::File out = LittleFS.open(kCsvFileTmp, "w");
  if (!out) {
    Serial.println("[HISTORY] logins.csv.tmp konnte nicht geschrieben werden (Pruning uebersprungen)");
    return;
  }
  out.println(kCsvHeader);
  for (size_t i = startIndex; i < kept.size(); i++) {
    out.println(kept[i]);
  }
  out.close();

  LittleFS.remove(kCsvFile);
  LittleFS.rename(kCsvFileTmp, kCsvFile);
}

void HistoryManager::clearHistory() {
  LittleFS.remove(kCsvFile);
  LittleFS.remove(kCsvFileTmp);
  _lastFlushedSequence = 0;
  _lastFlushMillis = millis();
}
