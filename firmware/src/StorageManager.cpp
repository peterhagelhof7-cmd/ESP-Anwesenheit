#include "StorageManager.h"

#include <Arduino.h>
#include <LittleFS.h>

bool StorageManager::begin() {
  if (!LittleFS.begin(true)) {
    Serial.println("[STORAGE] LittleFS konnte nicht gemountet werden!");
    return false;
  }
  Serial.println("[STORAGE] LittleFS gemountet");
  return true;
}
