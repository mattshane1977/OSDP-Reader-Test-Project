#include "app_console.h"
#include "mode_controller.h"
#include "leaf.h"
#include "nfc_hal.h"
#include "netconfig.h"
#include "osdp_pd.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_system.h"

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

static const char *TAG = "console";

/* ------- output helpers ------- */

static void out_line(const char *prefix, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void out_line(const char *prefix, const char *fmt, ...)
{
    va_list ap;
    printf("%s", prefix);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
}

#define OUT_OK(...)  out_line("OK ",  __VA_ARGS__)
#define OUT_ERR(...) out_line("ERR ", __VA_ARGS__)
#define OUT_RAW(...) out_line("",     __VA_ARGS__)

/* JSON event line — emitted from event_pump_task. */
static void emit_event_json(const reader_event_t *e)
{
    const char *kind = "?";
    switch (e->kind) {
    case EVT_CARD_READ_OK:   kind = "card_read";  break;
    case EVT_CARD_READ_FAIL: kind = "read_fail";  break;
    case EVT_CARD_WRITE_OK:  kind = "write_ok";   break;
    case EVT_CARD_WRITE_FAIL:kind = "write_fail"; break;
    case EVT_MODE_CHANGED:   kind = "mode";       break;
    }

    char uid_hex[24] = "";
    for (int i = 0; i < e->uid_len && i < 7; i++) {
        char b[3]; snprintf(b, sizeof(b), "%02x", e->uid[i]);
        strncat(uid_hex, b, sizeof(uid_hex) - strlen(uid_hex) - 1);
    }

    if (e->kind == EVT_CARD_READ_OK || e->kind == EVT_CARD_WRITE_OK) {
        printf("={\"type\":\"%s\",\"uid\":\"%s\",\"facility\":%u,\"id\":%lu,\"date\":%lu}\n",
               kind, uid_hex,
               (unsigned)e->cred.facility,
               (unsigned long)e->cred.card_id,
               (unsigned long)e->cred.issue_date);
    } else if (e->kind == EVT_MODE_CHANGED) {
        const char *m = (e->mode == READER_MODE_READ)  ? "read"
                      : (e->mode == READER_MODE_WRITE) ? "write" : "idle";
        printf("={\"type\":\"%s\",\"value\":\"%s\"}\n", kind, m);
    } else {
        printf("={\"type\":\"%s\",\"uid\":\"%s\"}\n", kind, uid_hex);
    }
    fflush(stdout);
}

static int split_args(char *line, char **argv, int max_argv)
{
    int argc = 0;
    char *p = line;
    while (*p && argc < max_argv) {
        while (*p && isspace((unsigned char)*p)) *p++ = '\0';
        if (!*p) break;
        argv[argc++] = p;
        while (*p && !isspace((unsigned char)*p)) p++;
    }
    return argc;
}

static int parse_hex_byte(const char *s)
{
    int hi, lo;
    if      (s[0] >= '0' && s[0] <= '9') hi = s[0] - '0';
    else if (s[0] >= 'a' && s[0] <= 'f') hi = s[0] - 'a' + 10;
    else if (s[0] >= 'A' && s[0] <= 'F') hi = s[0] - 'A' + 10;
    else return -1;
    if      (s[1] >= '0' && s[1] <= '9') lo = s[1] - '0';
    else if (s[1] >= 'a' && s[1] <= 'f') lo = s[1] - 'a' + 10;
    else if (s[1] >= 'A' && s[1] <= 'F') lo = s[1] - 'A' + 10;
    else return -1;
    return (hi << 4) | lo;
}

static int parse_hex_blob(const char *hex, uint8_t *out, size_t out_len)
{
    if (strlen(hex) != out_len * 2) return -1;
    for (size_t i = 0; i < out_len; i++) {
        int b = parse_hex_byte(&hex[i * 2]);
        if (b < 0) return -1;
        out[i] = (uint8_t)b;
    }
    return 0;
}

/* ------- commands ------- */

static void cmd_help(int argc, char **argv)
{
    (void)argc; (void)argv;
    OUT_RAW("commands:");
    OUT_RAW("  status");
    OUT_RAW("  self_test");
    OUT_RAW("  mode [idle|read|write]");
    OUT_RAW("  enroll <facility> <card_id> [issue_date]");
    OUT_RAW("  key set <32-hex-chars>");
    OUT_RAW("  key show");
    OUT_RAW("  osdp status");
    OUT_RAW("  osdp setup <addr> <baud>");
    OUT_RAW("  nfc status");
    OUT_RAW("  wifi setup <ssid> <pass>");
    OUT_RAW("  wifi status");
    OUT_RAW("  reboot");
    OUT_RAW("  help");
    OUT_OK("help");
}

static void cmd_self_test(int argc, char **argv)
{
    (void)argc; (void)argv;
    leaf_credential_t c1 = { .facility = 0x1234, .card_id = 0x567890AB, .issue_date = 0x11223344 };
    uint8_t pl[10];
    leaf_pack_credential(&c1, pl);

    leaf_credential_t c2 = {0};
    leaf_unpack_credential(pl, &c2);

    bool ok = (c1.facility == c2.facility) && (c1.card_id == c2.card_id) && (c1.issue_date == c2.issue_date);
    if (!ok) {
        OUT_ERR("self_test: pack/unpack mismatch");
        return;
    }

    uint8_t k[16] = {0};
    uint8_t m[8];
    leaf_payload_mac(k, pl, m);
    OUT_RAW("MAC: %02x%02x%02x%02x%02x%02x%02x%02x", m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7]);

    OUT_OK("self_test: logic verified");
}

static void cmd_status(int argc, char **argv)
{
    (void)argc; (void)argv;
    reader_mode_t m = mode_get();
    const char *ms = (m == READER_MODE_READ)  ? "read"
                   : (m == READER_MODE_WRITE) ? "write" : "idle";

    char ssid[33] = "";
    int  rssi = 0;
    esp_ip4_addr_t ip = {0};
    wifi_get_status(ssid, sizeof(ssid), &rssi, &ip);

    uint32_t uptime_s = (uint32_t)(esp_log_timestamp() / 1000);

    OUT_RAW("osdp-leaf-reader v1.0.0");
    OUT_RAW("mode    : %s", ms);
    OUT_RAW("uptime  : %lu s", (unsigned long)uptime_s);
    OUT_RAW("nfc     : %s (%s)", nfc_driver_name(), nfc_is_ready() ? "ready" : "not init");
    OUT_RAW("osdp    : %s", osdp_pd_is_online() ? "online (connected to CP)" : "offline");
    OUT_RAW("wifi    : %s rssi=%d ip=" IPSTR,
            ssid[0] ? ssid : "(disconnected)", rssi, IP2STR(&ip));
    OUT_OK("status");
}

static void cmd_mode(int argc, char **argv)
{
    if (argc < 2) {
        reader_mode_t m = mode_get();
        OUT_OK("mode is %s", (m == READER_MODE_READ)  ? "read"
                           : (m == READER_MODE_WRITE) ? "write" : "idle");
        return;
    }
    if      (!strcmp(argv[1], "idle"))  mode_set(READER_MODE_IDLE);
    else if (!strcmp(argv[1], "read"))  mode_set(READER_MODE_READ);
    else if (!strcmp(argv[1], "write")) mode_set(READER_MODE_WRITE);
    else {
        OUT_ERR("mode: unknown mode '%s'", argv[1]);
        return;
    }
    OUT_OK("mode set %s", argv[1]);
}

static void cmd_enroll(int argc, char **argv)
{
    if (argc < 3 || argc > 4) {
        OUT_ERR("enroll: usage: enroll <facility> <card_id> [issue_date]");
        return;
    }
    leaf_credential_t c = {0};
    c.facility    = (uint16_t)strtoul(argv[1], NULL, 0);
    c.card_id     = (uint32_t)strtoul(argv[2], NULL, 0);
    c.issue_date  = (argc == 4) ? (uint32_t)strtoul(argv[3], NULL, 0) : 0;

    mode_set_write_request(&c);
    mode_set(READER_MODE_WRITE);
    OUT_OK("enroll queued fac=%u id=%lu date=%lu — tap a card",
           (unsigned)c.facility, (unsigned long)c.card_id, (unsigned long)c.issue_date);
}

static void cmd_osdp(int argc, char **argv)
{
    if (argc < 2) { OUT_ERR("osdp: usage: osdp status | osdp setup <addr> <baud>"); return; }
    if (!strcmp(argv[1], "status")) {
        int addr = 0, baud = 0;
        osdp_pd_load_config(&addr, &baud);
        OUT_RAW("address : 0x%02x (%d)", addr, addr);
        OUT_RAW("baud    : %d", baud);
        OUT_RAW("status  : %s", osdp_pd_is_online() ? "ONLINE" : "OFFLINE");
        OUT_OK("osdp status");
        return;
    }
    if (!strcmp(argv[1], "setup")) {
        if (argc != 4) { OUT_ERR("osdp setup: usage: osdp setup <addr> <baud>"); return; }
        int addr = atoi(argv[2]);
        int baud = atoi(argv[3]);
        if (addr < 0 || addr > 126) { OUT_ERR("osdp setup: invalid address"); return; }
        if (osdp_pd_save_config(addr, baud) != ESP_OK) { OUT_ERR("osdp setup: nvs write failed"); return; }
        OUT_OK("osdp config saved — reboot to apply");
        return;
    }
    OUT_ERR("osdp: unknown subcommand '%s'", argv[1]);
}

static void cmd_nfc(int argc, char **argv)
{
    if (argc < 2) { OUT_ERR("nfc: usage: nfc status"); return; }
    if (!strcmp(argv[1], "status")) {
        OUT_RAW("driver  : %s", nfc_driver_name());
        OUT_RAW("ready   : %s", nfc_is_ready() ? "YES" : "NO");
        OUT_OK("nfc status");
        return;
    }
    OUT_ERR("nfc: unknown subcommand '%s'", argv[1]);
}

static void cmd_key(int argc, char **argv)
{
    if (argc < 2) { OUT_ERR("key: usage: key set <hex32> | key show"); return; }

    if (!strcmp(argv[1], "set")) {
        if (argc != 3) { OUT_ERR("key set: usage: key set <32-hex-chars>"); return; }
        uint8_t k[16];
        if (parse_hex_blob(argv[2], k, 16) != 0) {
            OUT_ERR("key set: not 32 hex chars");
            return;
        }
        if (leaf_set_site_key(k) != ESP_OK) { OUT_ERR("key set: nvs write failed"); return; }
        OUT_OK("key set (fp=%02x%02x%02x%02x)", k[0], k[1], k[2], k[3]);
        return;
    }
    if (!strcmp(argv[1], "show")) {
        uint8_t k[16];
        if (leaf_get_site_key(k) != ESP_OK) { OUT_ERR("key show: not set"); return; }
        OUT_OK("key fp=%02x%02x%02x%02x (full key not shown)",
               k[0], k[1], k[2], k[3]);
        return;
    }
    OUT_ERR("key: unknown subcommand '%s'", argv[1]);
}

static void cmd_wifi(int argc, char **argv)
{
    if (argc < 2) { OUT_ERR("wifi: usage: wifi setup|status"); return; }
    if (!strcmp(argv[1], "setup")) {
        if (argc != 4) { OUT_ERR("wifi setup: usage: wifi setup <ssid> <pass>"); return; }
        if (wifi_save_creds(argv[2], argv[3]) != ESP_OK) {
            OUT_ERR("wifi setup: nvs write failed");
            return;
        }
        OUT_OK("wifi creds saved — reboot to connect");
        return;
    }
    if (!strcmp(argv[1], "status")) {
        char ssid[33] = ""; int rssi = 0; esp_ip4_addr_t ip = {0};
        wifi_get_status(ssid, sizeof(ssid), &rssi, &ip);
        OUT_RAW("ssid : %s", ssid[0] ? ssid : "(disconnected)");
        OUT_RAW("rssi : %d", rssi);
        OUT_RAW("ip   : " IPSTR, IP2STR(&ip));
        OUT_OK("wifi status");
        return;
    }
    OUT_ERR("wifi: unknown subcommand '%s'", argv[1]);
}

static void cmd_reboot(int argc, char **argv)
{
    (void)argc; (void)argv;
    OUT_OK("rebooting...");
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
}

typedef struct {
    const char *name;
    void (*fn)(int argc, char **argv);
} cmd_t;

static const cmd_t s_cmds[] = {
    { "help",      cmd_help      },
    { "?",         cmd_help      },
    { "status",    cmd_status    },
    { "self_test", cmd_self_test },
    { "mode",      cmd_mode      },
    { "enroll",    cmd_enroll    },
    { "key",       cmd_key       },
    { "osdp",      cmd_osdp      },
    { "nfc",       cmd_nfc       },
    { "wifi",      cmd_wifi      },
    { "reboot",    cmd_reboot    },
};

static void dispatch(char *line)
{
    out_line("> ", "%s", line);
    char *argv[8];
    int argc = split_args(line, argv, 8);
    if (argc == 0) return;

    for (size_t i = 0; i < sizeof(s_cmds)/sizeof(s_cmds[0]); i++) {
        if (!strcmp(argv[0], s_cmds[i].name)) {
            s_cmds[i].fn(argc, argv);
            return;
        }
    }
    OUT_ERR("unknown command '%s' — try 'help'", argv[0]);
}

static void event_pump_task(void *arg)
{
    QueueHandle_t q = mode_subscribe("console");
    if (!q) {
        ESP_LOGE(TAG, "subscribe failed");
        vTaskDelete(NULL);
        return;
    }
    for (;;) {
        reader_event_t e;
        if (xQueueReceive(q, &e, portMAX_DELAY) == pdTRUE) {
            emit_event_json(&e);
        }
    }
}

static void console_task(void *arg)
{
    char line[128];
    OUT_RAW("osdp-leaf-reader console — type 'help'");

    for (;;) {
        if (!fgets(line, sizeof(line), stdin)) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        size_t n = strlen(line);
        while (n > 0 && (line[n-1] == '\n' || line[n-1] == '\r')) line[--n] = '\0';
        if (n == 0) continue;
        dispatch(line);
    }
}

void console_task_start(void)
{
    setvbuf(stdin, NULL, _IONBF, 0);
    xTaskCreatePinnedToCore(console_task,    "console", 4096, NULL, 4, NULL, 0);
    xTaskCreatePinnedToCore(event_pump_task, "evtpump", 3072, NULL, 4, NULL, 0);
}
