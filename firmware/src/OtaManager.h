#pragma once

#include <Arduino.h>

// Lokales OTA-Update per .bin-Upload, 1:1 aus dem sensormeter-Projekt
// uebernommen (inkl. des dort 2026-07-18 behobenen Chunk-Groessen-Bugs im
// Marker-Scan, siehe scanChunkForMarker()). Waehrend des Uploads wird im
// Byte-Stream nach einem einkompilierten Marker
// "SM-FW-ID:<FIRMWARE_PROJECT_ID>:<DEVICE_FIRMWARE_VERSION>:SM-FW-END"
// gesucht (siehe main.cpp), um zu verhindern, dass versehentlich eine .bin
// eines anderen Projekts oder eine aeltere eigene Version geflasht wird.

class OtaManager {
 public:
  bool beginLocalUpdate(size_t contentLength);
  bool writeLocalUpdateChunk(uint8_t* data, size_t len);
  bool endLocalUpdate();

  void setAllowDowngrade(bool allow) { _allowDowngrade = allow; }

  bool markerFound() const { return _markerFound; }
  bool identityMatches() const { return _identityMatches; }
  bool versionAllowed() const { return _versionAllowed; }

 private:
  bool _allowDowngrade = false;
  bool _markerFound = false;
  bool _identityMatches = false;
  bool _versionAllowed = false;

  // Rohe Byte-Puffer statt Arduino String: eine .bin enthaelt eingebettete
  // Null-Bytes, an denen String::indexOf() abbrechen wuerde (siehe
  // sensormeter/docs/entscheidungen.md).
  bool _capturing = false;
  static const size_t kTailCap = 16;
  uint8_t _tailBuf[kTailCap];
  size_t _tailLen = 0;
  static const size_t kCaptureCap = 128;
  uint8_t _captureBuf[kCaptureCap];
  size_t _captureLen = 0;

  void scanChunkForMarker(uint8_t* data, size_t len);
  void handleMarkerPayload(const String& payload);
};
