#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_log.h"
#include <string.h>

#include "osdp.h"

#include "osdp_pd.h"
#include "board.h"
#include "board_io.h"
#include "mode_controller.h"
#include "nvs.h"

static const char *OSDP_NVS_NS   = "osdp";
static const char *OSDP_NVS_ADDR = "address";
static const char *OSDP_NVS_BAUD = "baud";

static const char *TAG = "osdp";
static osdp_t *s_ctx;

bool osdp_pd_is_online(void)
{
    if (!s_ctx) return false;
    uint8_t status[4] = {0};
    osdp_get_status_mask(s_ctx, status);
    return (status[0] & 1) != 0;
}

esp_err_t osdp_pd_save_config(int address, int baud)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open(OSDP_NVS_NS, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    e = nvs_set_i32(h, OSDP_NVS_ADDR, address);
    if (e == ESP_OK) e = nvs_set_i32(h, OSDP_NVS_BAUD, baud);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e;
}

void osdp_pd_load_config(int *address, int *baud)
{
    nvs_handle_t h;
    *address = BOARD_OSDP_PD_ADDRESS;
    *baud    = BOARD_OSDP_BAUD;

    if (nvs_open(OSDP_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        int32_t a = 0, b = 0;
        if (nvs_get_i32(h, OSDP_NVS_ADDR, &a) == ESP_OK) *address = (int)a;
        if (nvs_get_i32(h, OSDP_NVS_BAUD, &b) == ESP_OK) *baud    = (int)b;
        nvs_close(h);
    }
}

/* ---- transport ---- */

static int uart_send(void *data, uint8_t *buf, int len)
{
    (void)data;
    return uart_write_bytes(BOARD_OSDP_UART_NUM, (const char *)buf, len);
}

static int uart_recv(void *data, uint8_t *buf, int len)
{
    (void)data;
    /* Non-blocking-ish: poll once with a tiny timeout. libosdp tolerates
     * fragmented reads. */
    int n = uart_read_bytes(BOARD_OSDP_UART_NUM, buf, len, pdMS_TO_TICKS(2));
    return n < 0 ? 0 : n;
}

static void uart_setup(int baud)
{
    const uart_config_t cfg = {
        .baud_rate = baud,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(BOARD_OSDP_UART_NUM, 1024, 1024, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(BOARD_OSDP_UART_NUM, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(BOARD_OSDP_UART_NUM,
                                 BOARD_OSDP_PIN_TX, BOARD_OSDP_PIN_RX,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
}

/* ---- libosdp PD command callbacks ----
 *
 * These run on whatever thread is calling osdp_pd_refresh(). Keep them short.
 * For LED/buzzer we just push to the indicator state machine.
 */

static int pd_cmd_handler(void *arg, struct osdp_cmd *cmd)
{
    (void)arg;
    switch (cmd->id) {
    case OSDP_CMD_LED: {
        const struct osdp_cmd_led *l = &cmd->led;
        /* libosdp's LED command is rich (flash patterns, on/off color); we map a
         * useful subset to our two LEDs. Common panel commands:
         *   color = OSDP_LED_COLOR_RED  -> indicate denied
         *   color = OSDP_LED_COLOR_GREEN -> indicate granted
         *   color = OSDP_LED_COLOR_NONE  -> back to armed
         */
        switch (l->permanent.on_color) {
        case OSDP_LED_COLOR_RED:    board_indicate(IND_DENY);  break;
        case OSDP_LED_COLOR_GREEN:  board_indicate(IND_GRANT); break;
        case OSDP_LED_COLOR_NONE:
        default:
            /* return to current armed state */
            switch (mode_get()) {
            case READER_MODE_READ:  board_indicate(IND_ARMED_READ);  break;
            case READER_MODE_WRITE: board_indicate(IND_ARMED_WRITE); break;
            default:                board_indicate(IND_IDLE);        break;
            }
            break;
        }
        break;
    }
    case OSDP_CMD_BUZZER: {
        const struct osdp_cmd_buzzer *b = &cmd->buzzer;
        if (b->control_code == 0) board_buzzer_set(false);
        else                       board_buzzer_set(true);
        break;
    }
    case OSDP_CMD_TEXT:
        ESP_LOGI(TAG, "TEXT cmd: '%.*s'", cmd->text.length, cmd->text.data);
        break;
    case OSDP_CMD_OUTPUT:
    case OSDP_CMD_KEYSET:
    default:
        break;
    }
    return 0;
}

/* Bridge: NFC events -> OSDP card_read events */

static void post_card_read(const reader_event_t *e)
{
    /* Build a raw cardread event with the credential payload (10 bytes).
     * facility(2) || card_id(4) || issue_date(4)
     */
    struct osdp_event ev = {0};
    ev.type = OSDP_EVENT_CARDREAD;
    ev.cardread.reader_no = 0;
    ev.cardread.format = OSDP_CARD_FMT_RAW_UNSPECIFIED;
    ev.cardread.direction = 0;
    ev.cardread.length = 10 * 8;   /* bits, per libosdp convention */

    uint8_t pl[10];
    pl[0] = (uint8_t)(e->cred.facility & 0xFF);
    pl[1] = (uint8_t)(e->cred.facility >> 8);
    pl[2] = (uint8_t)(e->cred.card_id & 0xFF);
    pl[3] = (uint8_t)((e->cred.card_id >> 8) & 0xFF);
    pl[4] = (uint8_t)((e->cred.card_id >> 16) & 0xFF);
    pl[5] = (uint8_t)((e->cred.card_id >> 24) & 0xFF);
    pl[6] = (uint8_t)(e->cred.issue_date & 0xFF);
    pl[7] = (uint8_t)((e->cred.issue_date >> 8) & 0xFF);
    pl[8] = (uint8_t)((e->cred.issue_date >> 16) & 0xFF);
    pl[9] = (uint8_t)((e->cred.issue_date >> 24) & 0xFF);
    memcpy(ev.cardread.data, pl, 10);

    osdp_pd_submit_event(s_ctx, &ev);
}

static void osdp_task(void *arg)
{
    int addr = 0, baud = 0;
    osdp_pd_load_config(&addr, &baud);

    uart_setup(baud);

    osdp_pd_info_t info = {
        .name = "esp32-leaf",
        .baud_rate = baud,
        .address = addr,
        .flags = 0,
        .id = {
            .version       = 1,
            .model         = 1,
            .vendor_code   = 0xACAB00,    /* placeholder — pick your own */
            .serial_number = 0x00000001,
            .firmware_version = 0x010000,
        },
        .cap = (struct osdp_pd_cap[]){
            { OSDP_PD_CAP_READER_LED_CONTROL,            1, 1 },
            { OSDP_PD_CAP_READER_AUDIBLE_OUTPUT,         1, 1 },
            { OSDP_PD_CAP_CARD_DATA_FORMAT,              1, 0 },
            { OSDP_PD_CAP_COMMUNICATION_SECURITY,        1, 1 },  /* Secure Channel */
            { -1, 0, 0 },
        },
        .channel = {
            .send  = uart_send,
            .recv  = uart_recv,
            .flush = NULL,
            .data  = NULL,
            .id    = 0,
        },
        /* SCBK: in production, generate per-PD and provision via panel.
         * For dev, set a known key here so the panel's "install mode" works.
         * libosdp will persist a rolled SCBK if the keyset command runs. */
        .scbk = (uint8_t[16]){
            0x30,0x31,0x32,0x33,0x34,0x35,0x36,0x37,
            0x38,0x39,0x41,0x42,0x43,0x44,0x45,0x46,
        },
    };

    s_ctx = osdp_pd_setup(&info);
    if (!s_ctx) {
        ESP_LOGE(TAG, "osdp_pd_setup failed");
        vTaskDelete(NULL);
        return;
    }
osdp_pd_set_command_callback(s_ctx, pd_cmd_handler, NULL);

QueueHandle_t evtq = mode_subscribe("osdp");
bool last_online = false;

for (;;) {
    osdp_pd_refresh(s_ctx);

    bool online = osdp_pd_is_online();
    if (online != last_online) {
        last_online = online;
        ESP_LOGI(TAG, "OSDP PD is now %s", online ? "ONLINE" : "OFFLINE");
        /* Post a mode event just to trigger a UI refresh */
        reader_event_t ev = { .kind = EVT_MODE_CHANGED, .mode = mode_get() };
        mode_post_event(&ev);
        }

        reader_event_t e;
        while (xQueueReceive(evtq, &e, 0) == pdTRUE) {            switch (e.kind) {
            case EVT_CARD_READ_OK:
                ESP_LOGI(TAG, "READ OK fac=%u id=%lu",
                         e.cred.facility, (unsigned long)e.cred.card_id);
                post_card_read(&e);
                break;
            case EVT_CARD_READ_FAIL:
                ESP_LOGW(TAG, "READ FAIL");
                /* No card_read event submitted — panel will see no read. */
                break;
            case EVT_CARD_WRITE_OK:
                ESP_LOGI(TAG, "WRITE OK fac=%u id=%lu",
                         e.cred.facility, (unsigned long)e.cred.card_id);
                break;
            case EVT_CARD_WRITE_FAIL:
                ESP_LOGW(TAG, "WRITE FAIL");
                break;
            case EVT_MODE_CHANGED:
                ESP_LOGI(TAG, "mode now %d", e.mode);
                break;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

void osdp_pd_task_start(void)
{
    xTaskCreatePinnedToCore(osdp_task, "osdp", 8192, NULL, 6, NULL, 0);
}
