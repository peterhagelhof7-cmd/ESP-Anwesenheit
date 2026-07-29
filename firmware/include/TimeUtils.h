#pragma once

#include <time.h>

// Heuristik "ist die Systemzeit per NTP synchronisiert" - identisch zum
// sensormeter-Projekt uebernommen: die ESP32-RTC startet nahe der Unix-Epoche
// 0, ein plausibles Datum liegt sicher nach dem 1.1.2001.
inline bool isTimeSynced() {
  return time(nullptr) > 978307200;
}
