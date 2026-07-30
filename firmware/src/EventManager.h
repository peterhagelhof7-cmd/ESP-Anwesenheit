#pragma once

#include <Arduino.h>
#include <freertos/semphr.h>
#include <vector>

// Kernstueck des Login-Monitors (siehe Projektbeschreibung.txt): nimmt
// Ereignisse von den Windows-Clients entgegen, haelt daraus den aktuellen
// Sitzungsstatus je Rechner sowie einen Ringpuffer der letzten Ereignisse -
// beides ausschliesslich im RAM (Projektbeschreibung: "Speicherung
// ausschliesslich im RAM"). Die 14-Tage-CSV-Historie liegt in HistoryManager.
//
// Thread-safe (Mutex) wie DataManager, da ESPAsyncWebServer-Handler in einem
// anderen Task als loop() laufen koennen.

// Server-Empfangszeit ist massgeblich fuer state/lastUpdate - der Zeitstempel
// im Client-Payload wird nur als Rohwert mitgefuehrt (Anzeige/Audit), da sich
// die Uhren vieler Windows-PCs nicht als synchron voraussetzen lassen und ein
// falsch gehender Client sonst die Statusanzeige verfaelschen wuerde. Siehe
// docs/entscheidungen.md.
struct LoginEvent {
  time_t serverTime = 0;
  unsigned long sequence = 0;
  String computer;
  String user;
  String event;            // roher event-Wert aus dem Client-Payload
  String logontype;        // "Local" | "RDP" | ""
  String clientTimestamp;  // roher timestamp-String aus dem Payload, nur zur Anzeige
};

struct ClientStatus {
  String computer;
  String user;
  String state;      // "Lokal" | "RDP" | "Gesperrt" | "Loginmaske"
  time_t lastUpdate = 0;
};

class EventManager {
 public:
  static const size_t RINGBUFFER_SIZE = 500;

  void begin();

  // Verarbeitet ein eingehendes Ereignis (POST /event). computer darf nicht
  // leer sein, event muss einer der in der Projektbeschreibung aufgefuehrten
  // Ereignistypen sein - liefert sonst false (WebServerManager antwortet
  // dann mit HTTP 400). logontype/timestamp sind optional.
  bool handleEvent(const String& computer, const String& user, const String& event, const String& logontype,
                    const String& clientTimestamp);

  // Aktueller Status aller bekannten Rechner, alphabetisch nach computer.
  size_t getStatuses(ClientStatus* out, size_t maxCount);
  size_t getStatusCount();

  // Anzahl Rechner mit state != "Loginmaske" (Lokal/RDP/Gesperrt zaehlen als
  // "angemeldet" - ein gesperrter Bildschirm hat weiterhin eine aktive
  // Benutzersitzung, nur "Loginmaske" bedeutet niemand angemeldet). Fuer
  // SNMP (.1.99999.3.1.0, siehe SNMPManager) - bewusst Gauge32-Semantik
  // (steigt und faellt), kein monotoner Counter, siehe docs/entscheidungen.md.
  size_t getLoggedInCount();

  // Letzte Ereignisse, neueste zuerst. filterComputer/filterUser leer =
  // kein Filter (Bonusziel "Filter nach Benutzer oder Rechner").
  size_t getEvents(LoginEvent* out, size_t maxCount, const String& filterComputer = "",
                    const String& filterUser = "");

  // Fuer HistoryManager: nur Ereignisse mit sequence > afterSequence,
  // chronologisch (aeltestes zuerst) - identisches Muster zu
  // DataManager::getLogEntriesAfter aus dem sensormeter-Projekt.
  size_t getEventsAfter(unsigned long afterSequence, LoginEvent* out, size_t maxCount);
  unsigned long getLastSequence();

  // Werksreset (scope=data): loescht Live-Status + Ringpuffer im RAM. Die
  // persistente logins.csv wird separat von HistoryManager geloescht.
  void clearAll();

  // Entfernt Status-Zeilen, deren lastUpdate laenger als maxAgeSeconds
  // zurueckliegt - unabhaengig vom state (auch "Lokal"/"RDP"/"Gesperrt",
  // nicht nur "Loginmaske"): ein Rechner, der sich einfach nie wieder
  // meldet, soll nicht dauerhaft in der Uebersicht haengen bleiben. Ohne
  // synchronisierte Uhr (isTimeSynced()) wird NICHT geprueft - sonst
  // koennte eine falsch gehende RTC kurz nach dem Boot versehentlich
  // alles als "uralt" werten und loeschen. Liefert die Anzahl entfernter
  // Zeilen (fuer eine Logmeldung im Aufrufer).
  size_t pruneStaleStatuses(unsigned long maxAgeSeconds);

 private:
  SemaphoreHandle_t _mutex = nullptr;

  LoginEvent _ring[RINGBUFFER_SIZE];
  size_t _ringCount = 0;
  size_t _ringNextIndex = 0;
  unsigned long _sequenceCounter = 0;

  std::vector<ClientStatus> _statuses;

  void applyToStatus(const String& computer, const String& user, const String& state, time_t when);
};
