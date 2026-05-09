#pragma once

/*
 * DeskBuddy — wifi_manager.h
 * Connects to WiFi using hardcoded credentials (extend to NVS later).
 * Blocks until connected or max retries exceeded.
 */

#include "esp_err.h"

/* Configure these before flashing */
#define WIFI_SSID      "SocLabs"
#define WIFI_PASSWORD  "Semuapastibisa007"
#define WIFI_MAX_RETRY 5

/**
 * @brief Initialise TCP/IP stack, create station, and connect.
 *        Blocks the calling task until IP is obtained or retries are exhausted.
 */
void wifi_manager_init(void);

/**
 * @brief Returns true if station is currently connected and has an IP.
 */
bool wifi_manager_is_connected(void);