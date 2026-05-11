#pragma once

/*
 * DeskBuddy — wifi_manager.h
 * - Boot: loads SSID/password from NVS, connects as station.
 * - No credentials or connect fails → starts softAP + captive portal.
 * - Portal: connect phone to "DeskBuddy-Setup" WiFi,
 *           open browser → 192.168.4.1 → enter SSID/password
 *           → saved to NVS → device reboots → connects automatically.
 * - Long press clock → manually trigger portal via wifi_manager_start_portal().
 */

#include <stdbool.h>

#define WIFI_AP_SSID            "Buddy-Setup"
#define WIFI_AP_PASSWORD        ""       /* open AP — no password */
#define WIFI_MAX_RETRY          5
#define WIFI_CONNECT_TIMEOUT_MS 15000

/**
 * @brief Load credentials from NVS and connect as station.
 *        If no credentials found, starts portal automatically.
 *        Blocks until connected, failed, or portal is active.
 */
void wifi_manager_init(void);

/**
 * @brief Manually start the softAP captive portal.
 *        Called by long press on clock screen.
 *        Saves credentials to NVS then reboots.
 */
void wifi_manager_start_portal(void);

/**
 * @brief Stop the softAP when exit the screen
 */
void wifi_manager_stop_portal(void);

/**
 * @brief Returns true if station is connected with an IP.
 */
bool wifi_manager_is_connected(void);