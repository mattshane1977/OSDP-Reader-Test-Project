#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include <string.h>

#include "board_io.h"
#include "leaf.h"
#include "mode_controller.h"
#include "nfc_hal.h"
#include "netconfig.h"
#include "app_console.h"
#include "webui.h"
#include "nfc_task.h"
#include "osdp_pd.h"

static const char *TAG = "main";

static void provision_default_site_key_if_missing(void)
{
    uint8_t k[16];
    if (leaf_get_site_key(k) == ESP_OK) return;
    /* DEV-ONLY default site key — replace via UI / serial console for prod */
    static const uint8_t dev_site_key[16] = {
        0xC0,0xFF,0xEE,0xC0,0xDE,0xCA,0xFE,0xBA,
        0xBE,0xDE,0xAD,0xBE,0xEF,0xFE,0xED,0xC0,
    };
    leaf_set_site_key(dev_site_key);
    ESP_LOGW(TAG, "provisioned dev default site key — replace before deploying");
}

void app_main(void)
{
    /* NVS first — encrypted partition holds site key + libosdp persistent
     * state + WiFi credentials */
    esp_err_t e;
#if defined(CONFIG_NVS_ENCRYPTION)
    ESP_LOGI(TAG, "Initializing encrypted NVS...");
    const esp_partition_t* key_part = esp_partition_find_first(
            ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS_KEYS, NULL);
    if (!key_part) {
        ESP_LOGE(TAG, "NVS key partition not found! Check partitions.csv");
        ESP_ERROR_CHECK(ESP_FAIL);
    }

    nvs_sec_cfg_t cfg;
    e = nvs_flash_read_security_cfg(key_part, &cfg);
    if (e == ESP_ERR_NVS_KEYS_NOT_INITIALIZED) {
        ESP_LOGI(TAG, "Generating new NVS encryption keys...");
        e = nvs_flash_generate_keys(key_part, &cfg);
        ESP_ERROR_CHECK(e);
    } else {
        ESP_ERROR_CHECK(e);
    }

    e = nvs_flash_secure_init(&cfg);
#else
    e = nvs_flash_init();
#endif

    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
#if defined(CONFIG_NVS_ENCRYPTION)
        e = nvs_flash_secure_init(&cfg);
#else
        e = nvs_flash_init();
#endif
    }
    ESP_ERROR_CHECK(e);

    board_init();

#if defined(CONFIG_NFC_DRIVER_PN532)
    pn532_driver_register();
#elif defined(CONFIG_NFC_DRIVER_PN5180)
    pn5180_driver_register();
#endif

    leaf_init();
    provision_default_site_key_if_missing();

    mode_init();

    /* Bring up the network before the web UI; console is independent and
     * can run even if WiFi never comes up — useful for serial-only debug. */
    if (netconfig_init() != ESP_OK) {
        ESP_LOGE(TAG, "netconfig failed; continuing without WiFi");
    }

    console_task_start();
    webui_start();

    /* Reader-side tasks last so the UI is responsive before NFC starts
     * polling for cards. */
    nfc_task_start();
    osdp_pd_task_start();

    ESP_LOGI(TAG, "system up — driver=%s wifi=%s",
             nfc_driver_name(),
             netconfig_sta_connected() ? "STA" :
             netconfig_in_ap_mode()    ? "AP"  : "down");
}
