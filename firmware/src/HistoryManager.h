#pragma once

#include <Arduino.h>
#include "DataManager.h"
#include "EventManager.h"

// Verdichtet den RAM-Ringpuffer aus EventManager inkrementell in eine
// persistente logins.csv auf LittleFS (14 Tage Vorhaltung, nach Login auf dem
// Webserver downloadbar). Das Flush-Intervall (kFlushIntervalMs) laeuft ab
// Boot (millis()). Ereignisse, die zwischen dem letzten Flush und einem
// Neustart nur im RAM standen, gehen verloren - das Intervall bestimmt also
// das Verlust-Zeitfenster bei einem Reboot.
//
// GEAENDERT 2026-08-03: von 24h auf 1h reduziert. Die urspruenglichen 24h
// (so in Projektbeschreibung.txt / docs/entscheidungen.md beschrieben)
// bedeuteten in der Praxis, dass die CSV bei jedem Neustart binnen 24h nie
// geschrieben wurde und die gesamte Historie verloren ging (real beobachtet).
// Da nur echte Session-Events in die CSV gehen (Heartbeats NICHT), ist
// haeufigeres Flushen quasi kostenlos fuers Flash. TODO: Projektbeschreibung/
// entscheidungen.md an diese Revision angleichen.
class HistoryManager {
 public:
  HistoryManager(EventManager& eventManager, DataManager& dataManager);

  void begin();
  void loop();

  // "/logins.csv" - als Funktion statt String-Literal-Konstante exportiert,
  // damit der eigentliche Dateiname (siehe .cpp, anonymer Namespace) an
  // einer einzigen Stelle steht (gleiches Muster wie DataManager.cpp im
  // sensormeter-Projekt) und keine static-constexpr-Zeiger-Definitionsfrage
  // ueber Uebersetzungseinheiten hinweg aufwirft.
  static const char* csvPath();

  // Werksreset (scope=data): loescht logins.csv, Ringpuffer/Status werden
  // vom Aufrufer separat ueber EventManager::clearAll() geleert.
  void clearHistory();

 private:
  static const unsigned long kFlushIntervalMs = 60UL * 60UL * 1000UL;  // 1h (war 24h) - siehe Kommentar oben
  static const int kRetentionDays = 14;
  // Sicherheitsdeckel unabhaengig von der 14-Tage-Regel, falls die Uhr nie
  // synchronisiert (dann kann nach Datum nicht sinnvoll geprueft werden) oder
  // ungewoehnlich viele Ereignisse anfallen - verhindert unbegrenztes
  // Wachstum der Datei.
  static const size_t kCsvMaxBytes = 512UL * 1024UL;

  EventManager& _events;
  DataManager& _data;
  unsigned long _lastFlushMillis = 0;
  unsigned long _lastFlushedSequence = 0;

  void flushToCsv();
  void pruneOldRows();
};
