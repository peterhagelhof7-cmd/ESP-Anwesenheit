#include "WebServerManager.h"

#include <ArduinoJson.h>
#include <LittleFS.h>
#include <Update.h>
#include <time.h>
#include <vector>
#include "TimeUtils.h"

#if __has_include("config.h")
#include "config.h"
#endif
#ifndef DEVICE_FIRMWARE_VERSION
#define DEVICE_FIRMWARE_VERSION "0.0.0"
#endif

namespace {
// Deutsche Kurzbezeichnung fuer die "Letzte Ereignisse"-Tabelle (siehe
// Projektbeschreibung.txt Beispieltabelle: "RDP Anmeldung", "Sperren",
// "Lokale Anmeldung").
String eventLabel(const String& event, const String& logontype) {
  if (event == "login") return (logontype == "RDP") ? "RDP Anmeldung" : "Lokale Anmeldung";
  if (event == "lock") return "Sperren";
  if (event == "unlock") return "Entsperren";
  if (event == "logout") return "Abgemeldet";
  if (event == "rdp-disconnect") return "RDP getrennt";
  if (event == "switch-to-rdp") return "Wechsel zu RDP";
  return event;
}

String formatTime(time_t t) {
  if (t == 0 || !isTimeSynced()) return "--:--:--";
  struct tm tmv;
  localtime_r(&t, &tmv);
  char buf[12];
  snprintf(buf, sizeof(buf), "%02d:%02d:%02d", tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
  return String(buf);
}
}  // namespace

WebServerManager::WebServerManager(DataManager& dataManager, ConfigManager& configManager,
                                    NetworkManager& networkManager, OtaManager& otaManager,
                                    EventManager& eventManager, HistoryManager& historyManager)
    : _data(dataManager),
      _config(configManager),
      _network(networkManager),
      _ota(otaManager),
      _events(eventManager),
      _history(historyManager),
      _server(80) {}

bool WebServerManager::checkAuth(AsyncWebServerRequest* request) {
  if (!request->authenticate("admin", _config.getConfig().settingsPassword.c_str())) {
    request->requestAuthentication("ESP-Anwesenheit (Benutzername: admin)");
    return false;
  }
  return true;
}

// ----------------------------------------------------------------------------
// Seiten-Grundgeruest - Stil an die Sensormeter-Familie angelehnt (Navy-
// Banner), eigener Blau-Akzent statt Orange, um beide Produktlinien optisch
// unterscheidbar zu halten. font-size per clamp() statt fest, damit sich die
// Seite spuerbar mit der Bildschirmbreite skaliert (Projektbeschreibung:
// "Die Seite skaliert mit der Aufloesung des Userclients").
// ----------------------------------------------------------------------------
String WebServerManager::buildPageShell(const String& title, const String& bodyContent) const {
  String html;
  html.reserve(bodyContent.length() + 1400);
  html += "<!DOCTYPE html><html lang=\"de\"><head><meta charset=\"UTF-8\">";
  html += "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">";
  html += "<title>" + title + "</title><style>";
  html += "*{box-sizing:border-box}";
  html += "body{background:#f5f7fa;color:#1c2430;font-size:clamp(13px,1.1vw + 10px,16px);text-align:center;"
          "font-family:-apple-system,'Segoe UI',Roboto,'Helvetica Neue',Arial,sans-serif;"
          "margin:0;padding:20px 14px 28px;line-height:1.5;}";
  html += "h1{font-size:clamp(18px,3vw,22px);background:#0f1f3d;color:#fff;margin:0 auto 18px;padding:18px 20px;"
          "border-radius:6px;max-width:820px;}";
  html += ".block{background:#fff;border:1px solid #dfe3ea;border-radius:6px;padding:14px 20px;"
          "margin:16px auto;max-width:820px;}";
  html += ".block h2{font-size:14px;color:#1f4e9c;margin:0 0 10px;padding-bottom:6px;"
          "border-bottom:2px solid #2a6fdb;text-transform:uppercase;letter-spacing:.04em;}";
  html += ".row{display:flex;justify-content:space-between;gap:16px;margin:8px 0;text-align:left;}";
  html += "p.hint{font-size:12.5px;color:#666f7d;text-align:left;margin:6px 0;}";
  html += "button,input[type=submit]{background:#2a6fdb;color:#fff;border:none;padding:9px 18px;"
          "font-size:14px;font-weight:600;border-radius:4px;cursor:pointer;margin:6px 4px;}";
  html += "button.danger{background:#c0392b;}";
  html += "button:hover,input[type=submit]:hover{opacity:.9;}";
  html += ".tablewrap{overflow-x:auto;max-width:100%;}";
  html += "table{margin:12px auto;border-collapse:collapse;font-size:13px;width:100%;min-width:420px;}";
  html += "td,th{border:1px solid #dfe3ea;padding:6px 12px;white-space:nowrap;}";
  html += "th{background:#eef1f6;}";
  html += "tr.stale{opacity:.5;font-style:italic;}";
  html += "input[type=text],input[type=password],input[type=file]{font-size:14px;padding:7px;width:80%;"
          "border:1px solid #cfd6e0;border-radius:4px;}";
  html += "select{font-size:14px;padding:7px;max-width:100%;border:1px solid #cfd6e0;border-radius:4px;}";
  html += "label{display:block;margin-top:10px;text-align:left;max-width:420px;margin-left:auto;"
          "margin-right:auto;font-size:13px;}";
  html += "a{color:#1f4e9c;text-decoration:none;}";
  html += "#scanResult div{cursor:pointer;padding:5px;font-size:13px;border-radius:3px;}";
  html += "#scanResult div:hover{background:#eef1f6;}";
  html += "</style></head><body>";
  html += bodyContent;
  html += "</body></html>";
  return html;
}

String WebServerManager::buildMainPageBody() const {
  const DeviceConfig& cfg = _config.getConfig();

  unsigned long uptimeSec = (unsigned long)(esp_timer_get_time() / 1000000ULL);
  char uptimeBuf[16];
  snprintf(uptimeBuf, sizeof(uptimeBuf), "%02lu:%02lu:%02lu", uptimeSec / 3600, (uptimeSec / 60) % 60,
            uptimeSec % 60);

  String html;
  html += "<h1>" + cfg.systemName + "</h1>";

  html += "<div class=\"block\"><h2>System</h2>";
  html += "<div class=\"row\"><span>Firmware</span><span>" DEVICE_FIRMWARE_VERSION "</span></div>";
  html += "<div class=\"row\"><span>Uptime</span><span>" + String(uptimeBuf) + "</span></div>";
  html += "<div class=\"row\"><span>LAN IP</span><span>" +
          (_network.isLanUp() ? _network.getLanIp().toString() : String("-")) + "</span></div>";
  html += "<div class=\"row\"><span>WLAN</span><span>" +
          (_network.isUsingFallbackWlan() ? String("Fallback-AP aktiv")
                                           : (_network.isWlanUp() ? _network.getWlanIp().toString() : String("-"))) +
          "</span></div>";
  html += "</div>";

  html += "<div class=\"block\"><h2>Aktive Rechner-Sitzungen</h2>";
  html += "<div class=\"tablewrap\"><table id=\"statusTable\"><tr><th>PC</th><th>Benutzer</th><th>Status</th>"
          "<th>Letzte \u00c4nderung</th></tr></table></div></div>";

  html += "<div class=\"block\"><h2>Letzte Ereignisse</h2>";
  html += "<form id=\"filterForm\" class=\"row\" style=\"flex-wrap:wrap\">";
  html += "<input type=\"text\" id=\"filterComputer\" placeholder=\"Filter: Rechner\" style=\"width:auto;flex:1\">";
  html += "<input type=\"text\" id=\"filterUser\" placeholder=\"Filter: Benutzer\" style=\"width:auto;flex:1\">";
  html += "<input type=\"submit\" value=\"Filtern\">";
  html += "</form>";
  html += "<div class=\"tablewrap\"><table id=\"eventsTable\"><tr><th>Zeit</th><th>PC</th><th>Benutzer</th>"
          "<th>Ereignis</th></tr></table></div></div>";

  html += "<div class=\"block\"><a href=\"/log.txt\"><button>Log</button></a>";
  html += "<a href=\"/settings\"><button>Einstellungen</button></a></div>";

  html += "<script>";
  html += "function loadStatus(){fetch('/status').then(r=>r.json()).then(list=>{";
  html += "let t=document.getElementById('statusTable');";
  html += "t.innerHTML='<tr><th>PC</th><th>Benutzer</th><th>Status</th><th>Letzte \\u00c4nderung</th></tr>';";
  html += "let now=Math.floor(Date.now()/1000);";
  html += "list.forEach(e=>{let r=t.insertRow();";
  html += "if(now-e.lastUpdateEpoch>900)r.className='stale';";
  html += "r.insertCell(0).innerText=e.computer;";
  html += "r.insertCell(1).innerText=e.user||'Kein Benutzer';";
  html += "r.insertCell(2).innerText=e.state;";
  html += "r.insertCell(3).innerText=e.lastUpdate;});});}";
  html += "function loadEvents(){let c=document.getElementById('filterComputer').value;";
  html += "let u=document.getElementById('filterUser').value;";
  html += "let qs=new URLSearchParams();if(c)qs.set('computer',c);if(u)qs.set('user',u);";
  html += "fetch('/events?'+qs.toString()).then(r=>r.json()).then(list=>{";
  html += "let t=document.getElementById('eventsTable');";
  html += "t.innerHTML='<tr><th>Zeit</th><th>PC</th><th>Benutzer</th><th>Ereignis</th></tr>';";
  html += "list.forEach(e=>{let r=t.insertRow();";
  html += "r.insertCell(0).innerText=e.time;";
  html += "r.insertCell(1).innerText=e.computer;";
  html += "r.insertCell(2).innerText=e.user||'Kein Benutzer';";
  html += "r.insertCell(3).innerText=e.event;});});}";
  html += "document.getElementById('filterForm').addEventListener('submit',ev=>{ev.preventDefault();loadEvents();});";
  html += "function refreshAll(){loadStatus();loadEvents();}";
  html += "refreshAll();setInterval(refreshAll,5000);";
  html += "</script>";

  return html;
}

String WebServerManager::buildSettingsPageBody() const {
  const DeviceConfig& cfg = _config.getConfig();

  String html;
  html += "<h1>Einstellungen</h1>";

  html += "<div class=\"block\"><h2>System</h2>";
  html += "<form method=\"POST\" action=\"/api/config\">";
  html += "<label>Systemname<input type=\"text\" name=\"systemName\" value=\"" + cfg.systemName + "\"></label>";
  html += "<label>Neues Passwort (leer = unveraendert)<input type=\"password\" name=\"newPassword\"></label>";
  html += "<input type=\"submit\" value=\"Speichern\"></form></div>";

  html += "<div class=\"block\"><h2>SNMP</h2>";
  html += "<form method=\"POST\" action=\"/api/config\">";
  html += "<label>Community<input type=\"text\" name=\"snmpCommunity\" value=\"" + cfg.snmpCommunity +
          "\"></label>";
  html += "<input type=\"submit\" value=\"Speichern\"></form>";
  html += "<p class=\"hint\">Read-only SNMP v1/v2c auf Port 161, siehe Zabbix-Template in docs/. "
          "Wirkt erst nach einem Neustart (Schaltflaeche unten).</p></div>";

  html += "<div class=\"block\"><h2>Anzeige</h2>";
  html += "<form method=\"POST\" action=\"/api/config\">";
  html += "<label>Veraltete Eintraege nach X Stunden entfernen (0 = nie)<input type=\"number\" min=\"0\" "
          "max=\"8760\" name=\"staleEntryHours\" value=\"" + String(cfg.staleEntryHours) + "\"></label>";
  html += "<input type=\"submit\" value=\"Speichern\"></form>";
  html += "<p class=\"hint\">Betrifft alle Status-Zeilen (auch Lokal/RDP/Gesperrt), nicht nur die Loginmaske - "
          "ein Rechner, der sich nie wieder meldet, bleibt sonst dauerhaft in der Uebersicht stehen.</p></div>";

  html += "<div class=\"block\"><h2>LAN</h2>";
  html += "<form method=\"POST\" action=\"/api/network/apply\">";
  html += "<input type=\"hidden\" name=\"iface\" value=\"lan\">";
  html += "<label><input type=\"checkbox\" name=\"dhcp\" " + String(cfg.lanDhcp ? "checked" : "") +
          "> DHCP</label>";
  html += "<label>IP<input type=\"text\" name=\"ip\" value=\"" + cfg.lanIp + "\"></label>";
  html += "<label>Netzmaske<input type=\"text\" name=\"mask\" value=\"" + cfg.lanMask + "\"></label>";
  html += "<label>Gateway<input type=\"text\" name=\"gateway\" value=\"" + cfg.lanGateway + "\"></label>";
  html += "<label>DNS (leer = Gateway)<input type=\"text\" name=\"dns\" value=\"" + cfg.lanDns + "\"></label>";
  html += "<input type=\"submit\" value=\"Uebernehmen &amp; neu starten\"></form>";
  html += "<p class=\"hint\">Nach dem Uebernehmen startet das Geraet neu und ist ggf. unter einer neuen "
          "Adresse erreichbar.</p></div>";

  html += "<div class=\"block\"><h2>WLAN</h2>";
  html += "<form method=\"POST\" action=\"/api/network/apply\">";
  html += "<input type=\"hidden\" name=\"iface\" value=\"wlan\">";
  html += "<label><input type=\"checkbox\" name=\"dhcp\" " + String(cfg.wlanDhcp ? "checked" : "") +
          "> DHCP</label>";
  html += "<label>IP<input type=\"text\" name=\"ip\" value=\"" + cfg.wlanIp + "\"></label>";
  html += "<label>Netzmaske<input type=\"text\" name=\"mask\" value=\"" + cfg.wlanMask + "\"></label>";
  html += "<label>Gateway<input type=\"text\" name=\"gateway\" value=\"" + cfg.wlanGateway + "\"></label>";
  html += "<label>DNS (leer = Gateway)<input type=\"text\" name=\"dns\" value=\"" + cfg.wlanDns + "\"></label>";
  html += "<input type=\"submit\" value=\"IP-Einstellungen uebernehmen &amp; neu starten\"></form>";

  html += "<form method=\"POST\" action=\"/api/wifi/connect\">";
  html += "<label>SSID<input type=\"text\" id=\"wlanSsid\" name=\"ssid\" value=\"" + cfg.wlanSsid + "\"></label>";
  html += "<button type=\"button\" onclick=\"scanWifi()\">SSIDs suchen</button><div id=\"scanResult\"></div>";
  html += "<label>PSK<input type=\"password\" name=\"psk\" value=\"" + cfg.wlanPsk + "\"></label>";
  html += "<input type=\"submit\" value=\"Verbinden &amp; testen (Neustart)\"></form></div>";

  html += "<div class=\"block\"><h2>Verlauf</h2>";
  html += "<a href=\"/logins.csv\"><button>logins.csv herunterladen</button></a>";
  html += "<p class=\"hint\">Enthaelt die letzten 14 Tage, taeglich aus dem Ringpuffer zusammengefuehrt.</p></div>";

  html += "<div class=\"block\"><h2>Firmware-Update (OTA)</h2>";
  html += "<form method=\"POST\" action=\"/api/ota/upload\" enctype=\"multipart/form-data\">";
  html += "<label><input type=\"checkbox\" name=\"otaForceDowngrade\"> Downgrade erzwingen</label>";
  html += "<input type=\"file\" name=\"file\" accept=\".bin\"><input type=\"submit\" value=\".bin hochladen\">";
  html += "</form></div>";

  html += "<div class=\"block\"><h2>Zuruecksetzen</h2>";
  html += "<form method=\"POST\" action=\"/api/factory-reset\" "
          "onsubmit=\"return confirm('Wirklich zuruecksetzen? Das Geraet startet danach neu.');\">";
  html += "<label>Umfang<select name=\"scope\">";
  html += "<option value=\"data\">Nur Daten (Ereignisse/Verlauf)</option>";
  html += "<option value=\"config\">Nur Einstellungen</option>";
  html += "<option value=\"all\">Alles</option>";
  html += "</select></label>";
  html += "<input type=\"submit\" class=\"danger\" value=\"Zuruecksetzen\"></form></div>";

  html += "<div class=\"block\"><form method=\"POST\" action=\"/api/reboot\">";
  html += "<input type=\"submit\" value=\"Neu starten\"></form>";
  html += "<a href=\"/\"><button>Zurueck</button></a></div>";

  html += "<div class=\"block\"><h2>Letzte Meldungen</h2>";
  html += "<div class=\"tablewrap\"><table id=\"logtable\"><tr><th>Zeit</th><th>Meldung</th></tr></table></div></div>";

  html += "<script>";
  html += "function scanWifi(){let d=document.getElementById('scanResult');d.innerText='Suche...';";
  html += "fetch('/api/wifi/scan').then(r=>r.json()).then(list=>{d.innerHTML='';";
  html += "list.forEach(n=>{let e=document.createElement('div');e.innerText=n.ssid+' ('+n.rssi+' dBm)';";
  html += "e.onclick=()=>{document.getElementById('wlanSsid').value=n.ssid;};d.appendChild(e);});});}";
  html += "fetch('/api/logs').then(r=>r.json()).then(d=>{let t=document.getElementById('logtable');";
  html += "d.entries.forEach(e=>{let r=t.insertRow();r.insertCell(0).innerText=e.time;"
          "r.insertCell(1).innerText=e.message;});});";
  html += "</script>";

  return html;
}

void WebServerManager::handleRoot(AsyncWebServerRequest* request) {
  request->send(200, "text/html", buildPageShell(_config.getConfig().systemName, buildMainPageBody()));
}

void WebServerManager::handleSettingsPage(AsyncWebServerRequest* request) {
  if (!checkAuth(request)) return;
  request->send(200, "text/html", buildPageShell("Einstellungen", buildSettingsPageBody()));
}

void WebServerManager::handleLoginsCsv(AsyncWebServerRequest* request) {
  if (!checkAuth(request)) return;
  if (!LittleFS.exists(HistoryManager::csvPath())) {
    request->send(404, "text/plain", "logins.csv existiert noch nicht (noch kein taeglicher Flush erfolgt)");
    return;
  }
  request->send(LittleFS, HistoryManager::csvPath(), "text/csv");
}

void WebServerManager::handleLogFile(AsyncWebServerRequest* request, const char* path) {
  if (!LittleFS.exists(path)) {
    request->send(404, "text/plain", "Nicht vorhanden");
    return;
  }
  request->send(LittleFS, path, "text/plain");
}

// ----------------------------------------------------------------------------
// POST /event
// ----------------------------------------------------------------------------
void WebServerManager::handleApiEventBody(AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index,
                                            size_t total) {
  // Sicherheitsdeckel: Ereignis-Payloads sind laut Projektbeschreibung wenige
  // Felder (computer/user/event/logontype/timestamp) - alles jenseits von
  // 4 KB ist entweder ein Fehlgebrauch der API oder ein Angriffsversuch und
  // wird verworfen (fuehrt unten zu einem JSON-Parse-Fehler -> HTTP 400).
  static const size_t kMaxBodyLen = 4096;
  String* body = reinterpret_cast<String*>(request->_tempObject);
  if (!body) {
    body = new String();
    body->reserve(min(total, kMaxBodyLen));
    request->_tempObject = body;
  }
  if (body->length() + len <= kMaxBodyLen) {
    body->concat(reinterpret_cast<const char*>(data), len);
  }
}

void WebServerManager::handleApiEvent(AsyncWebServerRequest* request) {
  String* body = reinterpret_cast<String*>(request->_tempObject);
  if (!body) {
    request->send(400, "application/json", "{\"ok\":false,\"error\":\"leerer Body\"}");
    return;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, *body);
  delete body;
  request->_tempObject = nullptr;

  if (err) {
    request->send(400, "application/json", "{\"ok\":false,\"error\":\"ungueltiges JSON\"}");
    return;
  }

  String computer = doc["computer"] | "";
  String user = doc["user"] | "";
  String event = doc["event"] | "";
  String logontype = doc["logontype"] | "";
  String timestamp = doc["timestamp"] | "";

  if (!_events.handleEvent(computer, user, event, logontype, timestamp)) {
    request->send(400, "application/json",
                   "{\"ok\":false,\"error\":\"computer fehlt oder event unbekannt\"}");
    return;
  }
  request->send(200, "application/json", "{\"ok\":true}");
}

void WebServerManager::handleApiStatus(AsyncWebServerRequest* request) {
  ClientStatus buf[128];
  size_t count = _events.getStatuses(buf, 128);

  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (size_t i = 0; i < count; i++) {
    JsonObject o = arr.add<JsonObject>();
    o["computer"] = buf[i].computer;
    o["user"] = buf[i].user;
    o["state"] = buf[i].state;
    o["lastUpdate"] = formatTime(buf[i].lastUpdate);
    o["lastUpdateEpoch"] = (uint32_t)buf[i].lastUpdate;
  }
  String out;
  serializeJson(doc, out);
  request->send(200, "application/json", out);
}

void WebServerManager::handleApiEvents(AsyncWebServerRequest* request) {
  String filterComputer = request->hasParam("computer") ? request->getParam("computer")->value() : "";
  String filterUser = request->hasParam("user") ? request->getParam("user")->value() : "";
  size_t limit = 100;
  if (request->hasParam("limit")) {
    long l = request->getParam("limit")->value().toInt();
    if (l > 0 && (size_t)l <= EventManager::RINGBUFFER_SIZE) limit = (size_t)l;
  }

  std::vector<LoginEvent> buf(limit);
  size_t count = _events.getEvents(buf.data(), limit, filterComputer, filterUser);

  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (size_t i = 0; i < count; i++) {
    JsonObject o = arr.add<JsonObject>();
    o["time"] = formatTime(buf[i].serverTime);
    o["computer"] = buf[i].computer;
    o["user"] = buf[i].user;
    o["event"] = eventLabel(buf[i].event, buf[i].logontype);
    o["eventRaw"] = buf[i].event;
    o["logontype"] = buf[i].logontype;
  }
  String out;
  serializeJson(doc, out);
  request->send(200, "application/json", out);
}

void WebServerManager::handleApiLogs(AsyncWebServerRequest* request) {
  LogEntry buf[DataManager::LOG_CAPACITY];
  size_t count = _data.getLogEntries(buf, DataManager::LOG_CAPACITY);

  JsonDocument doc;
  JsonArray arr = doc["entries"].to<JsonArray>();
  for (size_t i = 0; i < count; i++) {
    JsonObject o = arr.add<JsonObject>();
    o["time"] = formatTime(buf[i].timestamp);
    o["message"] = buf[i].message;
    o["severity"] = buf[i].severity;
  }
  String out;
  serializeJson(doc, out);
  request->send(200, "application/json", out);
}

// ----------------------------------------------------------------------------
// Einstellungen / Netzwerk / Reset / OTA - Formular-Handler (auth-geschuetzt)
// ----------------------------------------------------------------------------
void WebServerManager::handleApiConfigPost(AsyncWebServerRequest* request) {
  if (!checkAuth(request)) return;

  DeviceConfig cfg = _config.getConfig();
  if (request->hasParam("systemName", true)) {
    String name = request->getParam("systemName", true)->value();
    if (name.length() > 0) cfg.systemName = name;
  }
  if (request->hasParam("newPassword", true)) {
    String pw = request->getParam("newPassword", true)->value();
    if (pw.length() > 0) cfg.settingsPassword = pw;
  }
  if (request->hasParam("snmpCommunity", true)) {
    String community = request->getParam("snmpCommunity", true)->value();
    if (community.length() > 0) cfg.snmpCommunity = community;
  }
  if (request->hasParam("staleEntryHours", true)) {
    long hours = request->getParam("staleEntryHours", true)->value().toInt();
    if (hours >= 0 && hours <= 8760) cfg.staleEntryHours = (uint16_t)hours;
  }
  _config.setConfig(cfg);
  request->redirect("/settings");
}

void WebServerManager::handleApiReboot(AsyncWebServerRequest* request) {
  if (!checkAuth(request)) return;
  request->send(200, "text/plain", "Geraet startet neu...");
  _data.pushLogEntry("Manueller Neustart ueber Weboberflaeche");
  delay(300);
  ESP.restart();
}

void WebServerManager::handleApiWifiScan(AsyncWebServerRequest* request) {
  if (!checkAuth(request)) return;
  // Synchroner Scan (blockiert loop() fuer wenige Sekunden) - unkritisch,
  // da nur waehrend eines bewussten Einrichtungsvorgangs auf der
  // Einstellungsseite aufgerufen, nicht im Normalbetrieb.
  int n = WiFi.scanNetworks();
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (int i = 0; i < n; i++) {
    JsonObject o = arr.add<JsonObject>();
    o["ssid"] = WiFi.SSID(i);
    o["rssi"] = WiFi.RSSI(i);
  }
  WiFi.scanDelete();
  String out;
  serializeJson(doc, out);
  request->send(200, "application/json", out);
}

void WebServerManager::handleApiWifiConnect(AsyncWebServerRequest* request) {
  if (!checkAuth(request)) return;
  if (!request->hasParam("ssid", true)) {
    request->send(400, "text/plain", "ssid fehlt");
    return;
  }
  DeviceConfig cfg = _config.getConfig();
  cfg.wlanSsid = request->getParam("ssid", true)->value();
  cfg.wlanPsk = request->hasParam("psk", true) ? request->getParam("psk", true)->value() : "";
  cfg.wlanPendingTest = true;
  _config.setConfig(cfg);
  request->send(200, "text/plain", "WLAN-Zugangsdaten gesetzt, Geraet startet neu...");
  delay(300);
  ESP.restart();
}

void WebServerManager::handleApiNetworkApply(AsyncWebServerRequest* request) {
  if (!checkAuth(request)) return;
  if (!request->hasParam("iface", true)) {
    request->send(400, "text/plain", "iface fehlt");
    return;
  }
  String iface = request->getParam("iface", true)->value();
  bool isLan = (iface == "lan");
  bool dhcp = request->hasParam("dhcp", true);

  DeviceConfig cfg = _config.getConfig();
  if (dhcp) {
    if (isLan) {
      cfg.lanDhcp = true;
      cfg.lanIp = cfg.lanMask = cfg.lanGateway = cfg.lanDns = "";
    } else {
      cfg.wlanDhcp = true;
      cfg.wlanIp = cfg.wlanMask = cfg.wlanGateway = cfg.wlanDns = "";
    }
  } else {
    String ip = request->hasParam("ip", true) ? request->getParam("ip", true)->value() : "";
    String mask = request->hasParam("mask", true) ? request->getParam("mask", true)->value() : "";
    String gateway = request->hasParam("gateway", true) ? request->getParam("gateway", true)->value() : "";
    String dns = request->hasParam("dns", true) ? request->getParam("dns", true)->value() : "";

    IPAddress probe;
    if (!probe.fromString(ip) || !probe.fromString(mask) || !probe.fromString(gateway)) {
      request->send(400, "text/plain", "Ungueltige IP/Netzmaske/Gateway");
      return;
    }
    if (isLan) {
      cfg.lanDhcp = false;
      cfg.lanIp = ip;
      cfg.lanMask = mask;
      cfg.lanGateway = gateway;
      cfg.lanDns = dns;
    } else {
      cfg.wlanDhcp = false;
      cfg.wlanIp = ip;
      cfg.wlanMask = mask;
      cfg.wlanGateway = gateway;
      cfg.wlanDns = dns;
    }
  }
  _config.setConfig(cfg);
  request->send(200, "text/plain", "Netzwerkeinstellungen (" + iface + ") uebernommen, Geraet startet neu...");
  delay(300);
  ESP.restart();
}

void WebServerManager::handleApiFactoryReset(AsyncWebServerRequest* request) {
  if (!checkAuth(request)) return;
  String scope = request->hasParam("scope", true) ? request->getParam("scope", true)->value() : "all";

  if (scope == "config" || scope == "all") {
    _config.setConfig(DeviceConfig());
  }
  if (scope == "data" || scope == "all") {
    _events.clearAll();
    _history.clearHistory();
  }

  request->send(200, "text/plain", "Zurueckgesetzt (" + scope + "), Geraet startet neu...");
  delay(300);
  ESP.restart();
}

// ----------------------------------------------------------------------------
// OTA-Upload - Callback-Reihenfolge/Muster 1:1 aus dem sensormeter-Projekt
// uebernommen: _otaInProgress/_otaSuccess werden im Upload-Callback gesetzt
// und im anschliessend aufgerufenen Request-Callback ausgewertet.
// ----------------------------------------------------------------------------
void WebServerManager::handleOtaRequest(AsyncWebServerRequest* request) {
  if (!checkAuth(request)) return;
  if (_otaSuccess) {
    request->send(200, "text/plain", "Update erfolgreich, Geraet startet neu...");
    _data.pushLogEntry("OTA (lokaler Upload) erfolgreich, Neustart");
    delay(500);
    ESP.restart();
  } else if (!_otaInProgress) {
    _data.pushLogEntry("OTA (lokaler Upload) fehlgeschlagen (Schreibfehler)", DataManager::SEVERITY_ERROR);
    request->send(500, "text/plain", "Update fehlgeschlagen (Schreibfehler)");
  } else if (!_ota.markerFound()) {
    _data.pushLogEntry("OTA abgelehnt: kein Firmware-Erkennungsmerkmal gefunden", DataManager::SEVERITY_ERROR);
    request->send(400, "text/plain", "Update abgelehnt: kein gueltiges Firmware-Erkennungsmerkmal gefunden.");
  } else if (!_ota.identityMatches()) {
    _data.pushLogEntry("OTA abgelehnt: .bin gehoert zu einem anderen Projekt", DataManager::SEVERITY_ERROR);
    request->send(400, "text/plain", "Update abgelehnt: die Datei stammt von einem anderen Projekt.");
  } else if (!_ota.versionAllowed()) {
    _data.pushLogEntry("OTA abgelehnt: aeltere Firmware-Version", DataManager::SEVERITY_ERROR);
    request->send(400, "text/plain", "Update abgelehnt: aeltere Version (Downgrade nicht aktiviert).");
  } else {
    _data.pushLogEntry("OTA (lokaler Upload) fehlgeschlagen", DataManager::SEVERITY_ERROR);
    request->send(500, "text/plain", "Update fehlgeschlagen");
  }
}

void WebServerManager::handleOtaUpload(AsyncWebServerRequest* request, String filename, size_t index, uint8_t* data,
                                         size_t len, bool final) {
  if (!checkAuth(request)) return;
  if (index == 0) {
    _ota.setAllowDowngrade(request->hasParam("otaForceDowngrade", true));
    // request->contentLength() statt UPDATE_SIZE_UNKNOWN: ohne bekannte
    // Groesse legt Update.begin() die GESAMTE OTA-Partitionsgroesse als zu
    // loeschenden Bereich zugrunde statt nur der tatsaechlich benoetigten -
    // ein unnoetig langer blockierender Flash-Loeschvorgang gleich zu
    // Beginn des Uploads, der bei sensormeter-wlan (identisches Muster)
    // real einen WLAN-Verbindungsabbruch mitten im Upload ausgeloest hat.
    // Siehe docs/entscheidungen.md.
    _otaInProgress = _ota.beginLocalUpdate(request->contentLength());
    _otaSuccess = false;
  }
  if (_otaInProgress) {
    _otaInProgress = _ota.writeLocalUpdateChunk(data, len);
  }
  if (final && _otaInProgress) {
    _otaSuccess = _ota.endLocalUpdate();
  }
}

void WebServerManager::begin() {
  _server.on("/", HTTP_GET, [this](AsyncWebServerRequest* r) { handleRoot(r); });
  _server.on("/settings", HTTP_GET, [this](AsyncWebServerRequest* r) { handleSettingsPage(r); });
  _server.on("/logins.csv", HTTP_GET, [this](AsyncWebServerRequest* r) { handleLoginsCsv(r); });
  _server.on("/log.txt", HTTP_GET, [this](AsyncWebServerRequest* r) { handleLogFile(r, "/log.txt"); });
  _server.on("/log.old.txt", HTTP_GET, [this](AsyncWebServerRequest* r) { handleLogFile(r, "/log.old.txt"); });

  _server.on(
      "/event", HTTP_POST, [this](AsyncWebServerRequest* r) { handleApiEvent(r); }, nullptr,
      [this](AsyncWebServerRequest* r, uint8_t* data, size_t len, size_t index, size_t total) {
        handleApiEventBody(r, data, len, index, total);
      });

  _server.on("/status", HTTP_GET, [this](AsyncWebServerRequest* r) { handleApiStatus(r); });
  _server.on("/events", HTTP_GET, [this](AsyncWebServerRequest* r) { handleApiEvents(r); });
  _server.on("/api/logs", HTTP_GET, [this](AsyncWebServerRequest* r) { handleApiLogs(r); });

  _server.on("/api/config", HTTP_POST, [this](AsyncWebServerRequest* r) { handleApiConfigPost(r); });
  _server.on("/api/reboot", HTTP_POST, [this](AsyncWebServerRequest* r) { handleApiReboot(r); });
  _server.on("/api/wifi/scan", HTTP_GET, [this](AsyncWebServerRequest* r) { handleApiWifiScan(r); });
  _server.on("/api/wifi/connect", HTTP_POST, [this](AsyncWebServerRequest* r) { handleApiWifiConnect(r); });
  _server.on("/api/network/apply", HTTP_POST, [this](AsyncWebServerRequest* r) { handleApiNetworkApply(r); });
  _server.on("/api/factory-reset", HTTP_POST, [this](AsyncWebServerRequest* r) { handleApiFactoryReset(r); });

  _server.on(
      "/api/ota/upload", HTTP_POST, [this](AsyncWebServerRequest* r) { handleOtaRequest(r); },
      [this](AsyncWebServerRequest* r, String filename, size_t index, uint8_t* data, size_t len, bool final) {
        handleOtaUpload(r, filename, index, data, len, final);
      });

  _server.begin();
  Serial.println("[WEB] HTTP-Server gestartet (Port 80)");
}
