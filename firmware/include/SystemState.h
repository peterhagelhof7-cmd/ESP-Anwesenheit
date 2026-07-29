#pragma once

// Boot-Zustandsautomat, 1:1 aus dem sensormeter-Projekt uebernommen (siehe
// dortige docs/lastenheft.txt Abschnitt 8+12):
//
//   BOOT -> INIT -> NETWORK_CHECK
//     -> OK       -> RUN_NORMAL
//     -> FAIL     -> FALLBACK_MODE (eigener Access Point)
//   RUN_NORMAL:
//     NTP OK   -> bleibt RUN_NORMAL
//     NTP FAIL -> DHCP_TEST -> RESTORE_CONFIG -> ERROR_MODE

enum class SystemState {
  BOOT,
  INIT,
  NETWORK_CHECK,
  RUN_NORMAL,
  FALLBACK_MODE,
  DHCP_TEST,
  RESTORE_CONFIG,
  ERROR_MODE
};

inline const char* toString(SystemState state) {
  switch (state) {
    case SystemState::BOOT:            return "BOOT";
    case SystemState::INIT:            return "INIT";
    case SystemState::NETWORK_CHECK:   return "NETWORK_CHECK";
    case SystemState::RUN_NORMAL:      return "RUN_NORMAL";
    case SystemState::FALLBACK_MODE:   return "FALLBACK_MODE";
    case SystemState::DHCP_TEST:       return "DHCP_TEST";
    case SystemState::RESTORE_CONFIG:  return "RESTORE_CONFIG";
    case SystemState::ERROR_MODE:      return "ERROR_MODE";
    default:                           return "UNKNOWN";
  }
}
