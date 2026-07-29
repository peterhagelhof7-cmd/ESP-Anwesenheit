#include "OtaManager.h"

#include <Update.h>
#include <cstring>

#if __has_include("config.h")
#include "config.h"
#endif
#ifndef DEVICE_FIRMWARE_VERSION
#define DEVICE_FIRMWARE_VERSION "0.0.0"
#endif
#ifndef FIRMWARE_PROJECT_ID
#define FIRMWARE_PROJECT_ID "UNKNOWN"
#endif

namespace {
const char* const kMarkerPrefix = "SM-FW-ID:";
const size_t kMarkerPrefixLen = 9;
const char* const kMarkerSuffix = ":SM-FW-END";
const size_t kMaxCaptureLen = 96;

void parseVersion(const String& v, int& major, int& minor, int& patch, String& suffix) {
  major = minor = patch = 0;
  suffix = "";
  int dashPos = v.indexOf('-');
  String core = (dashPos >= 0) ? v.substring(0, dashPos) : v;
  if (dashPos >= 0) suffix = v.substring(dashPos + 1);
  int firstDot = core.indexOf('.');
  int secondDot = (firstDot >= 0) ? core.indexOf('.', firstDot + 1) : -1;
  if (firstDot < 0 || secondDot < 0) return;
  major = core.substring(0, firstDot).toInt();
  minor = core.substring(firstDot + 1, secondDot).toInt();
  patch = core.substring(secondDot + 1).toInt();
}

int compareVersions(const String& a, const String& b) {
  int aMajor, aMinor, aPatch, bMajor, bMinor, bPatch;
  String aSuffix, bSuffix;
  parseVersion(a, aMajor, aMinor, aPatch, aSuffix);
  parseVersion(b, bMajor, bMinor, bPatch, bSuffix);
  if (aMajor != bMajor) return aMajor - bMajor;
  if (aMinor != bMinor) return aMinor - bMinor;
  if (aPatch != bPatch) return aPatch - bPatch;
  if (aSuffix.length() == 0 && bSuffix.length() > 0) return 1;
  if (aSuffix.length() > 0 && bSuffix.length() == 0) return -1;
  return aSuffix.compareTo(bSuffix);
}
}  // namespace

namespace {
int findBytes(const uint8_t* haystack, size_t haystackLen, const char* needle, size_t needleLen) {
  if (needleLen == 0 || haystackLen < needleLen) return -1;
  for (size_t i = 0; i + needleLen <= haystackLen; i++) {
    if (memcmp(haystack + i, needle, needleLen) == 0) return (int)i;
  }
  return -1;
}
}  // namespace

bool OtaManager::beginLocalUpdate(size_t contentLength) {
  _markerFound = false;
  _identityMatches = false;
  _versionAllowed = false;
  _capturing = false;
  _tailLen = 0;
  _captureLen = 0;
  return Update.begin(contentLength);
}

bool OtaManager::writeLocalUpdateChunk(uint8_t* data, size_t len) {
  if (!_markerFound) scanChunkForMarker(data, len);
  return Update.write(data, len) == len;
}

// Durchsucht "data"/"len" direkt (keine Groessenbeschraenkung) statt eines
// gedeckelten Zwischenpuffers - siehe sensormeter/docs/entscheidungen.md
// "OTA-Marker-Scan-Chunk-Groessen-Bug" fuer den Befund, der diese Fassung
// noetig gemacht hat (echte AsyncWebServer-Upload-Chunks sind oft groesser
// als ein kleiner fester Puffer, der Marker wurde dadurch nie gefunden).
void OtaManager::scanChunkForMarker(uint8_t* data, size_t len) {
  if (!_capturing) {
    bool spansTail = false;
    size_t afterPrefixInData = 0;
    if (_tailLen > 0) {
      uint8_t joinBuf[kTailCap + kMarkerPrefixLen];
      size_t headLen = len < kMarkerPrefixLen ? len : kMarkerPrefixLen;
      memcpy(joinBuf, _tailBuf, _tailLen);
      memcpy(joinBuf + _tailLen, data, headLen);
      size_t joinLen = _tailLen + headLen;
      int p = findBytes(joinBuf, joinLen, kMarkerPrefix, kMarkerPrefixLen);
      if (p >= 0 && (size_t)p < _tailLen) {
        spansTail = true;
        afterPrefixInData = (size_t)p + kMarkerPrefixLen - _tailLen;
      }
    }

    int prefixPos = spansTail ? -1 : findBytes(data, len, kMarkerPrefix, kMarkerPrefixLen);

    if (!spansTail && prefixPos < 0) {
      size_t keep = kMarkerPrefixLen > 0 ? kMarkerPrefixLen - 1 : 0;
      if (keep > kTailCap) keep = kTailCap;
      size_t start = len > keep ? len - keep : 0;
      _tailLen = len - start;
      memcpy(_tailBuf, data + start, _tailLen);
      return;
    }
    _capturing = true;
    size_t afterPrefix = spansTail ? afterPrefixInData : (size_t)prefixPos + kMarkerPrefixLen;
    size_t remaining = len - afterPrefix;
    if (remaining > kCaptureCap) remaining = kCaptureCap;
    memcpy(_captureBuf, data + afterPrefix, remaining);
    _captureLen = remaining;
    _tailLen = 0;
  } else {
    size_t copyLen = len;
    if (_captureLen + copyLen > kCaptureCap) copyLen = kCaptureCap - _captureLen;
    memcpy(_captureBuf + _captureLen, data, copyLen);
    _captureLen += copyLen;
  }

  int suffixPos = findBytes(_captureBuf, _captureLen, kMarkerSuffix, strlen(kMarkerSuffix));
  if (suffixPos >= 0) {
    String payload;
    payload.reserve(suffixPos);
    for (int i = 0; i < suffixPos; i++) payload += (char)_captureBuf[i];
    handleMarkerPayload(payload);
    _capturing = false;
    _captureLen = 0;
    _tailLen = 0;
    return;
  }
  if (_captureLen >= kCaptureCap) {
    _capturing = false;
    _captureLen = 0;
    _tailLen = 0;
  }
}

void OtaManager::handleMarkerPayload(const String& payload) {
  int sep = payload.indexOf(':');
  if (sep < 0) return;
  String projectId = payload.substring(0, sep);
  String version = payload.substring(sep + 1);

  _markerFound = true;
  _identityMatches = (projectId == FIRMWARE_PROJECT_ID);
  _versionAllowed = _allowDowngrade || compareVersions(version, DEVICE_FIRMWARE_VERSION) >= 0;
}

bool OtaManager::endLocalUpdate() {
  if (!_markerFound || !_identityMatches || !_versionAllowed) {
    Update.abort();
    return false;
  }
  return Update.end(true);
}
