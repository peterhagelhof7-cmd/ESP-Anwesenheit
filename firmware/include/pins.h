#pragma once

// ============================================================================
// WT32-ETH01 - Pinbelegung (ESP-Anwesenheit)
// Nur Ethernet wird von diesem Projekt genutzt - kein RJ45-Modularanschluss,
// keine Sensorik, kein Relais (anders als die Sensormeter-Familie).
// ============================================================================

// --- Ethernet PHY (LAN8720) - fest verdrahtet, NICHT aendern -----------------
#define ETH_PHY_TYPE   ETH_PHY_LAN8720
#define ETH_PHY_ADDR   1
#define ETH_PHY_MDC    23
#define ETH_PHY_MDIO   18
#define ETH_PHY_POWER  16
#define ETH_CLK_MODE   ETH_CLOCK_GPIO0_IN
