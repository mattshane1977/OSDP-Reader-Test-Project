#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

/*
 * PN532 driver — SPI mode.
 *
 * Strategy: only InListPassiveTarget (find a card) and InDataExchange (raw
 * APDU pass-through). All DESFire-specific logic lives in the desfire
 * component. The PN532 doesn't do AES, so we use it as a dumb ISO 14443-4
 * tunnel and run the EV2 secure messaging on the ESP32 itself.
 *
 * Frame size: PN532 InDataExchange tops out around 263 data bytes per call.
 * DESFire commands fit comfortably; for large file reads we chunk in the
 * desfire layer using ReadData with offset/length.
 */

esp_err_t pn532_init(void);
esp_err_t pn532_wake(void);

/* SAM config — must run once after wake */
esp_err_t pn532_sam_config_normal(void);

/* Look for a single ISO14443A target. Returns ESP_OK and fills uid/uid_len
 * if a card is present; ESP_ERR_NOT_FOUND if the field is empty. */
esp_err_t pn532_find_target(uint8_t *uid, size_t uid_buf_len,
                            uint8_t *uid_len, uint8_t *sak);

/* Send raw APDU to the active target (Tg=1). Caller owns both buffers. */
esp_err_t pn532_apdu_exchange(const uint8_t *tx, size_t tx_len,
                              uint8_t *rx, size_t rx_buf_len, size_t *rx_len);

/* Drop the active target so we don't hold the field on between polls. */
esp_err_t pn532_release(void);
