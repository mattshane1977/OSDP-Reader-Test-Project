#pragma once

#include "esp_err.h"
#include <stddef.h>
#include <stdbool.h>

/*
 * WiFi bring-up.
 *
 * On boot:
 *   - Read SSID/pass from NVS namespace "wifi" (written by `wifi setup`
 *     console command).
 *   - If present, connect as STA. Auto-reconnect on disconnect.
 *   - If absent, bring up a SoftAP named "leaf-reader-XXXXXX" (where the
 *     suffix is the last 3 bytes of the MAC) so the user can reach the
 *     web UI to provision. Open AP, no password — dev convenience only.
 *
 * The function blocks the caller for up to ~10s waiting for a STA
 * connection to settle; if the connection times out it falls through to
 * AP fallback. Returns ESP_OK once *some* network interface is up.
 */

#include "esp_err.h"
#include "esp_netif.h"
#include <stddef.h>
#include <stdbool.h>

/* ... existing netconfig_init etc ... */
esp_err_t netconfig_init(void);
bool      netconfig_sta_connected(void);
bool      netconfig_in_ap_mode(void);

/* Management API */
esp_err_t wifi_save_creds(const char *ssid, const char *pass);
void      wifi_get_status(char *ssid_out, size_t ssid_buf,
                          int *rssi_out, esp_ip4_addr_t *ip_out);
