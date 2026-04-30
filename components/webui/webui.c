#include "webui.h"
#include "mode_controller.h"
#include "leaf.h"
#include "nfc_hal.h"
#include "osdp_pd.h"

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_netif.h"
#include "esp_wifi.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>
#include <stdlib.h>
#include <ctype.h>

static const char *TAG = "webui";

/* The embedded index.html — names come from EMBED_FILES in CMakeLists.txt.
 * The macro expansion produces _binary_index_html_start / _end symbols. */
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");

static httpd_handle_t s_server = NULL;
static int            s_ws_fd  = -1;     /* single connected client */

/* ------- index page ------- */

static esp_err_t handle_index(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    /* The browser caches aggressively; during dev it's helpful to disable */
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, (const char *)index_html_start,
                           index_html_end - index_html_start);
}

/* ------- WebSocket -------
 *
 * Tiny hand-rolled JSON subset because pulling cJSON for ~5 commands isn't
 * worth the binary cost. Format is restricted: top-level object, all values
 * either string or integer. Quoted strings can't contain escapes.
 *
 * Each parser is a forgiving "find this key, parse next value" search. Not
 * a real JSON parser; sufficient for our messages.
 */

static const char *find_key(const char *json, const char *key)
{
    /* Look for "key" with surrounding quotes, then skip past the colon. */
    char needle[32];
    int n = snprintf(needle, sizeof(needle), "\"%s\"", key);
    if (n <= 0 || n >= (int)sizeof(needle)) return NULL;
    const char *p = strstr(json, needle);
    if (!p) return NULL;
    p += n;
    while (*p && (*p == ' ' || *p == ':' || *p == '\t')) p++;
    return *p ? p : NULL;
}

static bool get_str(const char *json, const char *key, char *out, size_t out_len)
{
    const char *p = find_key(json, key);
    if (!p || *p != '"') return false;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i < out_len - 1) out[i++] = *p++;
    out[i] = '\0';
    return *p == '"';
}

static bool get_int(const char *json, const char *key, long *out)
{
    const char *p = find_key(json, key);
    if (!p) return false;
    char *end = NULL;
    long v = strtol(p, &end, 0);
    if (end == p) return false;
    *out = v;
    return true;
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

/* ------- WS send helpers ------- */

static esp_err_t ws_send_text(int fd, const char *payload)
{
    if (!s_server || fd < 0) return ESP_ERR_INVALID_STATE;
    httpd_ws_frame_t f = {
        .final = true,
        .fragmented = false,
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)payload,
        .len = strlen(payload),
    };
    return httpd_ws_send_frame_async(s_server, fd, &f);
}

static void ws_ack(int fd, const char *cmd, const char *detail)
{
    char buf[160];
    snprintf(buf, sizeof(buf), "{\"type\":\"ack\",\"cmd\":\"%s\",\"detail\":\"%s\"}",
             cmd, detail ? detail : "");
    ws_send_text(fd, buf);
}

static void ws_error(int fd, const char *msg)
{
    char buf[160];
    snprintf(buf, sizeof(buf), "{\"type\":\"error\",\"message\":\"%s\"}", msg);
    ws_send_text(fd, buf);
}

static void ws_send_status(int fd)
{
    reader_mode_t m = mode_get();
    const char *ms = (m == READER_MODE_READ)  ? "read"
                   : (m == READER_MODE_WRITE) ? "write" : "idle";

    esp_netif_t *nif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip = {0};
    if (nif) esp_netif_get_ip_info(nif, &ip);

    char buf[250];
    snprintf(buf, sizeof(buf),
             "{\"type\":\"status\",\"mode\":\"%s\",\"driver\":\"%s\","
             "\"osdp\":\"%s\",\"ip\":\"" IPSTR "\"}",
             ms, nfc_driver_name(),
             osdp_pd_is_online() ? "online" : "offline",
             IP2STR(&ip.ip));
    ws_send_text(fd, buf);
}

/* ------- WS command handler ------- */

static void handle_ws_command(int fd, const char *json)
{
    char cmd[16] = {0};
    if (!get_str(json, "cmd", cmd, sizeof(cmd))) {
        ws_error(fd, "missing cmd");
        return;
    }

    if (!strcmp(cmd, "mode")) {
        char val[16] = {0};
        if (!get_str(json, "value", val, sizeof(val))) {
            ws_error(fd, "mode: missing value"); return;
        }
        if      (!strcmp(val, "idle"))  mode_set(READER_MODE_IDLE);
        else if (!strcmp(val, "read"))  mode_set(READER_MODE_READ);
        else if (!strcmp(val, "write")) mode_set(READER_MODE_WRITE);
        else { ws_error(fd, "mode: unknown value"); return; }
        ws_ack(fd, "mode", val);
        return;
    }

    if (!strcmp(cmd, "enroll")) {
        long fac = 0, id = 0, date = 0;
        if (!get_int(json, "facility", &fac) || !get_int(json, "id", &id)) {
            ws_error(fd, "enroll: need facility and id"); return;
        }
        get_int(json, "date", &date);   /* optional */
        leaf_credential_t c = {
            .facility = (uint16_t)fac,
            .card_id  = (uint32_t)id,
            .issue_date = (uint32_t)date,
        };
        mode_set_write_request(&c);
        mode_set(READER_MODE_WRITE);
        ws_ack(fd, "enroll", "queued; tap card");
        return;
    }

    if (!strcmp(cmd, "key_set")) {
        char hex[33] = {0};
        if (!get_str(json, "key", hex, sizeof(hex)) || strlen(hex) != 32) {
            ws_error(fd, "key_set: need 32 hex chars"); return;
        }
        uint8_t k[16];
        for (int i = 0; i < 16; i++) {
            int b = parse_hex_byte(&hex[i * 2]);
            if (b < 0) { ws_error(fd, "key_set: bad hex"); return; }
            k[i] = (uint8_t)b;
        }
        if (leaf_set_site_key(k) != ESP_OK) {
            ws_error(fd, "key_set: nvs write failed"); return;
        }
        ws_ack(fd, "key_set", "ok");
        return;
    }

    if (!strcmp(cmd, "osdp_set")) {
        long addr = 0, baud = 0;
        if (!get_int(json, "address", &addr) || !get_int(json, "baud", &baud)) {
            ws_error(fd, "osdp_set: need address and baud"); return;
        }
        if (addr < 0 || addr > 126) {
            ws_error(fd, "osdp_set: invalid address"); return;
        }
        if (osdp_pd_save_config((int)addr, (int)baud) != ESP_OK) {
            ws_error(fd, "osdp_set: nvs write failed"); return;
        }
        ws_ack(fd, "osdp_set", "ok; reboot to apply");
        return;
    }

    if (!strcmp(cmd, "reboot")) {
        ws_ack(fd, "reboot", "in 100ms");
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_restart();
        return;
    }

    ws_error(fd, "unknown cmd");
}

static esp_err_t handle_ws(httpd_req_t *req)
{
    /* Handshake: req->method == HTTP_GET means upgrade request */
    if (req->method == HTTP_GET) {
        s_ws_fd = httpd_req_to_sockfd(req);
        ESP_LOGI(TAG, "ws client connected fd=%d", s_ws_fd);
        /* The send-status will fire from the client first message handler,
         * since httpd doesn't allow async sends until the handshake is
         * actually complete. Schedule a one-shot delayed send instead. */
        return ESP_OK;
    }

    /* Subsequent frames */
    httpd_ws_frame_t f = {0};
    /* Probe for length first */
    esp_err_t e = httpd_ws_recv_frame(req, &f, 0);
    if (e != ESP_OK) return e;

    if (f.len == 0 || f.len > 1024) return ESP_OK;

    uint8_t *buf = malloc(f.len + 1);
    if (!buf) return ESP_ERR_NO_MEM;
    f.payload = buf;
    e = httpd_ws_recv_frame(req, &f, f.len);
    if (e == ESP_OK && f.type == HTTPD_WS_TYPE_TEXT) {
        buf[f.len] = '\0';
        s_ws_fd = httpd_req_to_sockfd(req);
        handle_ws_command(s_ws_fd, (char *)buf);
    }
    free(buf);
    return ESP_OK;
}

/* ------- event push task ------- */

static void uid_to_hex(const uint8_t *uid, uint8_t len, char *out, size_t out_buf)
{
    out[0] = '\0';
    if (len > 7) len = 7;
    for (int i = 0; i < len; i++) {
        char b[3]; snprintf(b, sizeof(b), "%02x", uid[i]);
        strncat(out, b, out_buf - strlen(out) - 1);
    }
}

static void event_push_task(void *arg)
{
    QueueHandle_t q = mode_subscribe("webui");
    if (!q) {
        ESP_LOGE(TAG, "subscribe failed");
        vTaskDelete(NULL);
        return;
    }

    /* Send an initial status message ~1s after boot to populate any
     * already-connected client. Loop will keep sending these on demand
     * via mode-changed events. */
    vTaskDelay(pdMS_TO_TICKS(1000));
    if (s_ws_fd >= 0) ws_send_status(s_ws_fd);

    for (;;) {
        reader_event_t e;
        if (xQueueReceive(q, &e, portMAX_DELAY) != pdTRUE) continue;
        if (s_ws_fd < 0) continue;

        char uid[24];
        uid_to_hex(e.uid, e.uid_len, uid, sizeof(uid));

        char buf[256];
        switch (e.kind) {
        case EVT_CARD_READ_OK:
            snprintf(buf, sizeof(buf),
                     "{\"type\":\"card_read\",\"uid\":\"%s\","
                     "\"facility\":%u,\"id\":%lu,\"date\":%lu}",
                     uid, (unsigned)e.cred.facility,
                     (unsigned long)e.cred.card_id,
                     (unsigned long)e.cred.issue_date);
            break;
        case EVT_CARD_READ_FAIL:
            snprintf(buf, sizeof(buf),
                     "{\"type\":\"read_fail\",\"uid\":\"%s\"}", uid);
            break;
        case EVT_CARD_WRITE_OK:
            snprintf(buf, sizeof(buf),
                     "{\"type\":\"write_ok\",\"uid\":\"%s\","
                     "\"facility\":%u,\"id\":%lu,\"date\":%lu}",
                     uid, (unsigned)e.cred.facility,
                     (unsigned long)e.cred.card_id,
                     (unsigned long)e.cred.issue_date);
            break;
        case EVT_CARD_WRITE_FAIL:
            snprintf(buf, sizeof(buf),
                     "{\"type\":\"write_fail\",\"uid\":\"%s\"}", uid);
            break;
        case EVT_MODE_CHANGED: {
            const char *m = (e.mode == READER_MODE_READ)  ? "read"
                          : (e.mode == READER_MODE_WRITE) ? "write" : "idle";
            snprintf(buf, sizeof(buf),
                     "{\"type\":\"mode\",\"value\":\"%s\"}", m);
            break;
        }
        default:
            continue;
        }

        if (ws_send_text(s_ws_fd, buf) != ESP_OK) {
            /* Send failed — client probably gone. Drop fd; will be set
             * again on next connect. */
            ESP_LOGI(TAG, "ws send failed; dropping fd %d", s_ws_fd);
            s_ws_fd = -1;
        }
    }
}

/* ------- start ------- */

void webui_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.lru_purge_enable = true;
    cfg.max_open_sockets = 4;

    if (httpd_start(&s_server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed");
        return;
    }

    httpd_uri_t index = {
        .uri = "/", .method = HTTP_GET,
        .handler = handle_index, .user_ctx = NULL,
    };
    httpd_register_uri_handler(s_server, &index);

    httpd_uri_t ws = {
        .uri = "/ws", .method = HTTP_GET,
        .handler = handle_ws, .user_ctx = NULL,
        .is_websocket = true,
    };
    httpd_register_uri_handler(s_server, &ws);

    xTaskCreatePinnedToCore(event_push_task, "wspush", 4096, NULL, 4, NULL, 0);

    ESP_LOGI(TAG, "web UI on http://<device-ip>/");
}
