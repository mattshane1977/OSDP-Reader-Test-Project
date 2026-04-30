#pragma once

/*
 * Serial console — line-based dev/diagnostic interface on UART0.
 *
 * Coexists with esp_log: log lines pass through unchanged; command results
 * use a 'OK ' / 'ERR ' prefix, and machine-readable events are emitted with
 * a leading '=' followed by JSON. A simple host-side parser can grep for
 * '=' to harvest events.
 *
 * Available commands (run `help` for the live list):
 *   help                       — print command summary
 *   status                     — current mode, driver, IP, uptime
 *   mode [idle|read|write]     — get or set reader mode
 *   enroll <fac> <id> [date]   — queue a personalize-on-next-tap
 *   key set <hex32>            — replace site key (32 hex chars = 16 bytes)
 *   key show                   — print site key fingerprint (first 4 bytes)
 *   wifi setup <ssid> <pass>   — provision WiFi credentials
 *   wifi status                — IP, RSSI, SSID
 *   reboot                     — software restart
 */

void console_task_start(void);

/* Console emits events via mode_subscribe(); call after mode_init(). */
