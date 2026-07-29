#pragma once

// LittleFS-Zugriff (config.json, logins.csv, log.txt). Identisch zum
// sensormeter-Projekt.

class StorageManager {
 public:
  bool begin();  // true = LittleFS erfolgreich gemountet
};
