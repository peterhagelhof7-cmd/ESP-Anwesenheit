#pragma once

#include <Arduino.h>
#include "DataManager.h"
#include "EventManager.h"

// Verdichtet den RAM-Ringpuffer aus EventManager alle 24h in eine
// persistente logins.csv auf LittleFS (Projektbeschreibung: "alle 24h
// zusammenfuehren ... welche 14 Tage die Werte haelt, und nach Login auf dem
// Webserver downloadbar ist"). Die 24h-Uhr laeuft ab Boot (millis()) - nach
// einem Neustart beginnt sie bewusst neu ("nach einem Neustart beginnt die
// taegliche Historie neu", siehe Projektbeschreibung.txt): Ereignisse, die
// zwischen dem letzten Flush und einem Neustart nur im RAM standen, gehen
// dabei verloren - das ist explizit so spezifiziert, nicht ein Bug. Siehe
// docs/entscheidungen.md fuer die Abwaegung ("500er-Ringpuffer vs.
// taeglicher Flush").
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
  static const unsigned long kFlushIntervalMs = 24UL * 60UL * 60UL * 1000UL;
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
