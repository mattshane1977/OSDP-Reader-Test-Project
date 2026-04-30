#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include <string.h>

#include "nfc_task.h"
#include "board_io.h"
#include "nfc_hal.h"
#include "desfire.h"
#include "leaf.h"
#include "mode_controller.h"

static const char *TAG = "nfc";

static void nfc_task(void *arg)
{
    ESP_ERROR_CHECK(nfc_init());
    desfire_init();

    /* Card present-edge detection: we want exactly one read/write per
     * presentation, not a tight loop while the card is held in the field. */
    bool card_in_field = false;
    int  empty_count   = 0;

    for (;;) {
        /* Service the local mode button each iteration */
        button_event_t btn = board_button_poll();
        if (btn != BTN_NONE) mode_handle_button(btn);

        uint8_t uid[10] = {0}; uint8_t uid_len = 0; uint8_t sak = 0;
        esp_err_t e = nfc_find_target(uid, sizeof(uid), &uid_len, &sak);

        if (e == ESP_OK && !card_in_field) {
            card_in_field = true;
            empty_count = 0;

            ESP_LOGI(TAG, "card UID %02x%02x%02x%02x%s",
                     uid[0], uid[1], uid[2], uid[3],
                     uid_len > 4 ? "..." : "");

            reader_mode_t m = mode_get();
            reader_event_t out = { .uid_len = uid_len };
            memcpy(out.uid, uid, uid_len > 7 ? 7 : uid_len);

            if (m == READER_MODE_READ) {
                leaf_credential_t cred = {0};
                if (leaf_read(uid, uid_len, &cred) == ESP_OK) {
                    out.kind = EVT_CARD_READ_OK;
                    out.cred = cred;
                    out.mode = m;
                    mode_post_event(&out);
                    board_indicate(IND_GRANT);
                } else {
                    out.kind = EVT_CARD_READ_FAIL;
                    out.mode = m;
                    mode_post_event(&out);
                    board_indicate(IND_DENY);
                }
            } else if (m == READER_MODE_WRITE) {
                leaf_credential_t to_write;
                if (!mode_take_write_request(&to_write)) {
                    /* Nothing queued — produce a dummy so personalize still
                     * has something to write. In practice the UI will
                     * populate this. */
                    to_write.facility = 0x0001;
                    to_write.card_id  = 0xCAFEBABE;
                    to_write.issue_date = 0;
                }
                if (leaf_personalize(uid, uid_len, &to_write) == ESP_OK) {
                    out.kind = EVT_CARD_WRITE_OK;
                    out.cred = to_write;
                    out.mode = m;
                    mode_post_event(&out);
                    board_indicate(IND_WROTE_OK);
                } else {
                    out.kind = EVT_CARD_WRITE_FAIL;
                    out.mode = m;
                    mode_post_event(&out);
                    board_indicate(IND_ERROR);
                }
            }
            /* IDLE mode: detect the card so it shows up in logs but do nothing */

            nfc_release();
        } else if (e == ESP_ERR_NOT_FOUND) {
            if (card_in_field) {
                if (++empty_count > 3) { card_in_field = false; }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(150));
    }
}

void nfc_task_start(void)
{
    xTaskCreatePinnedToCore(nfc_task, "nfc", 8192, NULL, 5, NULL, 1);
}
