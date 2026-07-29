#include "EventManager.h"

#include <algorithm>

namespace {
// Bildet die 7 in der Projektbeschreibung aufgefuehrten Ereignistypen auf den
// resultierenden Sitzungsstatus ab. Rueckgabe false = unbekannter event-Wert
// (wird von handleEvent() als ungueltige Anfrage behandelt).
//
// "rdp-disconnect" faellt auf "Lokal" zurueck: die Windows-Ereignisse melden
// nur das Trennen der RDP-Sitzung, nicht ob der Rechner danach an der Konsole
// tatsaechlich lokal weiterbenutzt wird oder an der Loginmaske steht - "Lokal"
// ist die Annahme, die der bisherigen RDP-Sitzung am naechsten kommt, ohne
// eine dritte Zwischenzustandslogik einzufuehren. Siehe docs/entscheidungen.md.
bool mapEventToState(const String& event, const String& logontype, String& outState, bool& outClearUser) {
  outClearUser = false;
  if (event == "login") {
    outState = (logontype == "RDP") ? "RDP" : "Lokal";
    return true;
  }
  if (event == "unlock") {
    outState = (logontype == "RDP") ? "RDP" : "Lokal";
    return true;
  }
  if (event == "lock") {
    outState = "Gesperrt";
    return true;
  }
  if (event == "logout") {
    outState = "Loginmaske";
    outClearUser = true;
    return true;
  }
  if (event == "rdp-disconnect") {
    outState = "Lokal";
    return true;
  }
  if (event == "switch-to-rdp") {
    outState = "RDP";
    return true;
  }
  return false;
}
}  // namespace

void EventManager::begin() {
  _mutex = xSemaphoreCreateMutex();
}

void EventManager::applyToStatus(const String& computer, const String& user, const String& state, time_t when) {
  for (auto& s : _statuses) {
    if (s.computer == computer) {
      s.user = user;
      s.state = state;
      s.lastUpdate = when;
      return;
    }
  }
  ClientStatus s;
  s.computer = computer;
  s.user = user;
  s.state = state;
  s.lastUpdate = when;
  _statuses.push_back(s);
}

bool EventManager::handleEvent(const String& computer, const String& user, const String& event,
                                const String& logontype, const String& clientTimestamp) {
  if (computer.length() == 0) return false;

  String state;
  bool clearUser = false;
  if (!mapEventToState(event, logontype, state, clearUser)) return false;

  time_t now = time(nullptr);

  xSemaphoreTake(_mutex, portMAX_DELAY);
  applyToStatus(computer, clearUser ? String("") : user, state, now);

  _sequenceCounter++;
  LoginEvent& e = _ring[_ringNextIndex];
  e.serverTime = now;
  e.sequence = _sequenceCounter;
  e.computer = computer;
  e.user = user;
  e.event = event;
  e.logontype = logontype;
  e.clientTimestamp = clientTimestamp;
  _ringNextIndex = (_ringNextIndex + 1) % RINGBUFFER_SIZE;
  if (_ringCount < RINGBUFFER_SIZE) _ringCount++;
  xSemaphoreGive(_mutex);

  return true;
}

size_t EventManager::getStatusCount() {
  size_t count;
  xSemaphoreTake(_mutex, portMAX_DELAY);
  count = _statuses.size();
  xSemaphoreGive(_mutex);
  return count;
}

size_t EventManager::getLoggedInCount() {
  size_t count = 0;
  xSemaphoreTake(_mutex, portMAX_DELAY);
  for (const auto& s : _statuses) {
    if (s.state != "Loginmaske") count++;
  }
  xSemaphoreGive(_mutex);
  return count;
}

size_t EventManager::getStatuses(ClientStatus* out, size_t maxCount) {
  size_t count;
  xSemaphoreTake(_mutex, portMAX_DELAY);
  count = min(_statuses.size(), maxCount);
  // Alphabetisch nach Rechnername - stabile, nachvollziehbare Reihenfolge in
  // der Weboberflaeche statt Einfuegereihenfolge.
  std::vector<ClientStatus> sorted = _statuses;
  xSemaphoreGive(_mutex);

  std::sort(sorted.begin(), sorted.end(),
            [](const ClientStatus& a, const ClientStatus& b) { return a.computer < b.computer; });
  for (size_t i = 0; i < count; i++) out[i] = sorted[i];
  return count;
}

size_t EventManager::getEvents(LoginEvent* out, size_t maxCount, const String& filterComputer,
                                const String& filterUser) {
  size_t count = 0;
  xSemaphoreTake(_mutex, portMAX_DELAY);
  for (size_t i = 0; i < _ringCount && count < maxCount; i++) {
    size_t index = (_ringNextIndex + RINGBUFFER_SIZE - 1 - i) % RINGBUFFER_SIZE;
    const LoginEvent& e = _ring[index];
    if (filterComputer.length() > 0 && e.computer != filterComputer) continue;
    if (filterUser.length() > 0 && e.user != filterUser) continue;
    out[count++] = e;
  }
  xSemaphoreGive(_mutex);
  return count;
}

size_t EventManager::getEventsAfter(unsigned long afterSequence, LoginEvent* out, size_t maxCount) {
  size_t count = 0;
  xSemaphoreTake(_mutex, portMAX_DELAY);
  size_t startIndex = (_ringNextIndex + RINGBUFFER_SIZE - _ringCount) % RINGBUFFER_SIZE;
  for (size_t i = 0; i < _ringCount && count < maxCount; i++) {
    size_t index = (startIndex + i) % RINGBUFFER_SIZE;
    if (_ring[index].sequence > afterSequence) {
      out[count++] = _ring[index];
    }
  }
  xSemaphoreGive(_mutex);
  return count;
}

unsigned long EventManager::getLastSequence() {
  unsigned long seq;
  xSemaphoreTake(_mutex, portMAX_DELAY);
  seq = _sequenceCounter;
  xSemaphoreGive(_mutex);
  return seq;
}

void EventManager::clearAll() {
  xSemaphoreTake(_mutex, portMAX_DELAY);
  _statuses.clear();
  _ringCount = 0;
  _ringNextIndex = 0;
  xSemaphoreGive(_mutex);
}
