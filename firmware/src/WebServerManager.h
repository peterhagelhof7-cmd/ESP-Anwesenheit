#pragma once

#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include "ConfigManager.h"
#include "DataManager.h"
#include "EventManager.h"
#include "HistoryManager.h"
#include "NetworkManager.h"
#include "OtaManager.h"

// Stellt die Weboberflaeche (Live-Status/Ereignisse, Einstellungen) und die
// HTTP-API bereit (Projektbeschreibung.txt "HTTP-API"/"Weboberflaeche").
// Async (ESPAsyncWebServer), Port 80 - Auth-/OTA-/Reset-Muster 1:1 aus dem
// sensormeter-Projekt uebernommen (siehe docs/entscheidungen.md).
class WebServerManager {
 public:
  WebServerManager(DataManager& dataManager, ConfigManager& configManager, NetworkManager& networkManager,
                    OtaManager& otaManager, EventManager& eventManager, HistoryManager& historyManager);

  void begin();

 private:
  DataManager& _data;
  ConfigManager& _config;
  NetworkManager& _network;
  OtaManager& _ota;
  EventManager& _events;
  HistoryManager& _history;
  AsyncWebServer _server;

  bool _otaInProgress = false;
  bool _otaSuccess = false;

  bool checkAuth(AsyncWebServerRequest* request);

  String buildPageShell(const String& title, const String& bodyContent) const;
  String buildMainPageBody() const;
  String buildSettingsPageBody() const;

  void handleRoot(AsyncWebServerRequest* request);
  void handleSettingsPage(AsyncWebServerRequest* request);
  void handleLoginsCsv(AsyncWebServerRequest* request);
  void handleLogFile(AsyncWebServerRequest* request, const char* path);

  // POST /event liefert einen JSON-Body statt Formularfeldern - Body wird
  // per onBody-Callback in request->_tempObject aufgesammelt (Standardmuster
  // fuer ESPAsyncWebServer, siehe docs/entscheidungen.md) und hier geparst.
  void handleApiEventBody(AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total);
  void handleApiEvent(AsyncWebServerRequest* request);

  void handleApiStatus(AsyncWebServerRequest* request);
  void handleApiEvents(AsyncWebServerRequest* request);
  void handleApiLogs(AsyncWebServerRequest* request);

  void handleApiConfigPost(AsyncWebServerRequest* request);
  void handleApiReboot(AsyncWebServerRequest* request);
  void handleApiWifiScan(AsyncWebServerRequest* request);
  void handleApiWifiConnect(AsyncWebServerRequest* request);
  void handleApiNetworkApply(AsyncWebServerRequest* request);
  void handleApiFactoryReset(AsyncWebServerRequest* request);

  void handleOtaRequest(AsyncWebServerRequest* request);
  void handleOtaUpload(AsyncWebServerRequest* request, String filename, size_t index, uint8_t* data, size_t len,
                        bool final);
};
