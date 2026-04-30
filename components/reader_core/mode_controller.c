#include "mode_controller.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_system.h"
#include <stdatomic.h>
#include <string.h>

static const char *TAG = "mode";

static _Atomic reader_mode_t s_mode = READER_MODE_IDLE;

#define MAX_SUBSCRIBERS 4
typedef struct {
    QueueHandle_t q;
    const char   *name;
    uint32_t      drops;
} subscriber_t;
static subscriber_t s_subs[MAX_SUBSCRIBERS];
static int          s_sub_count = 0;

static portMUX_TYPE s_wr_lock = portMUX_INITIALIZER_UNLOCKED;
static reader_write_request_t s_write_req;

void mode_init(void)
{
    memset(s_subs, 0, sizeof(s_subs));
    s_sub_count = 0;
    s_write_req.pending = false;
    mode_set(READER_MODE_IDLE);
}

reader_mode_t mode_get(void) { return atomic_load(&s_mode); }

static void apply_indicator(reader_mode_t m)
{
    switch (m) {
    case READER_MODE_IDLE:  board_indicate(IND_IDLE);        break;
    case READER_MODE_READ:  board_indicate(IND_ARMED_READ);  break;
    case READER_MODE_WRITE: board_indicate(IND_ARMED_WRITE); break;
    }
}

void mode_set(reader_mode_t m)
{
    reader_mode_t prev = atomic_exchange(&s_mode, m);
    if (prev != m) {
        ESP_LOGI(TAG, "mode %d -> %d", prev, m);
        apply_indicator(m);
        reader_event_t e = { .kind = EVT_MODE_CHANGED, .mode = m };
        mode_post_event(&e);
    }
}

void mode_handle_button(button_event_t btn)
{
    if (btn == BTN_SHORT) {
        reader_mode_t m = mode_get();
        if (m == READER_MODE_READ)  mode_set(READER_MODE_IDLE);
        else                        mode_set(READER_MODE_READ);
    } else if (btn == BTN_LONG) {
        reader_mode_t m = mode_get();
        if (m == READER_MODE_WRITE) mode_set(READER_MODE_IDLE);
        else                         mode_set(READER_MODE_WRITE);
    } else if (btn == BTN_FACTORY_RESET) {
        ESP_LOGW(TAG, "FACTORY RESET TRIGGERED");
        board_indicate(IND_ERROR);
        vTaskDelay(pdMS_TO_TICKS(1000));
        nvs_flash_erase();
        esp_restart();
    }
}

QueueHandle_t mode_subscribe(const char *name)
{
    if (s_sub_count >= MAX_SUBSCRIBERS) {
        ESP_LOGE(TAG, "subscribe: max %d reached, refusing '%s'",
                 MAX_SUBSCRIBERS, name ? name : "(anon)");
        return NULL;
    }
    QueueHandle_t q = xQueueCreate(8, sizeof(reader_event_t));
    if (!q) return NULL;
    s_subs[s_sub_count].q     = q;
    s_subs[s_sub_count].name  = name;
    s_subs[s_sub_count].drops = 0;
    s_sub_count++;
    ESP_LOGI(TAG, "subscriber %d: %s", s_sub_count - 1, name ? name : "(anon)");
    return q;
}

QueueHandle_t mode_event_queue(void)
{
    return s_sub_count > 0 ? s_subs[0].q : NULL;
}

void mode_post_event(const reader_event_t *e)
{
    for (int i = 0; i < s_sub_count; i++) {
        if (xQueueSend(s_subs[i].q, e, 0) != pdTRUE) {
            s_subs[i].drops++;
            /* Don't log on every drop — would flood at high event rates.
             * Instead log every 32nd drop per subscriber. */
            if ((s_subs[i].drops & 0x1F) == 1) {
                ESP_LOGW(TAG, "subscriber '%s' dropped event (total drops: %lu)",
                         s_subs[i].name ? s_subs[i].name : "?",
                         (unsigned long)s_subs[i].drops);
            }
        }
    }
}

void mode_set_write_request(const leaf_credential_t *c)
{
    portENTER_CRITICAL(&s_wr_lock);
    s_write_req.cred = *c;
    s_write_req.pending = true;
    portEXIT_CRITICAL(&s_wr_lock);
}

bool mode_take_write_request(leaf_credential_t *out)
{
    bool ok = false;
    portENTER_CRITICAL(&s_wr_lock);
    if (s_write_req.pending) {
        *out = s_write_req.cred;
        s_write_req.pending = false;
        ok = true;
    }
    portEXIT_CRITICAL(&s_wr_lock);
    return ok;
}
