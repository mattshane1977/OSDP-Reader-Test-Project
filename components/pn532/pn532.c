#include "pn532.h"
#include "board.h"
#include "sdkconfig.h"

#if defined(CONFIG_NFC_DRIVER_PN532)

#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

#include <string.h>

static const char *TAG = "pn532";

/*
 * SPI mode quirks worth knowing:
 *   - PN532 in SPI mode wants LSB-first. ESP32 SPI master can do that natively
 *     via SPI_DEVICE_BIT_LSBFIRST.
 *   - First byte of every transaction is a status/data direction byte:
 *       0x01 = data write to PN532
 *       0x02 = read response
 *       0x02 also used to poll status (0x01 = ready)
 *   - Mode 0 (CPOL=0, CPHA=0). Up to 5 MHz on most boards.
 *
 * Frame format (normal information frame):
 *   00 00 FF LEN LCS TFI PD0..PDn DCS 00
 *   where TFI = 0xD4 host->PN532 or 0xD5 PN532->host
 *         LCS = 0x100 - LEN  (low byte)
 *         DCS = 0x100 - sum(TFI + PD0..PDn) (low byte)
 *
 * After every command the PN532 sends a 6-byte ACK frame:
 *   00 00 FF 00 FF 00
 * We wait for ACK, then poll status until 0x01, then read the response.
 */

#define PN532_PREAMBLE      0x00
#define PN532_STARTCODE1    0x00
#define PN532_STARTCODE2    0xFF
#define PN532_POSTAMBLE     0x00

#define PN532_HOSTTOPN532   0xD4
#define PN532_PN532TOHOST   0xD5

#define PN532_SPI_DATAWRITE 0x01
#define PN532_SPI_STATREAD  0x02
#define PN532_SPI_DATAREAD  0x03

#define CMD_GETFIRMWAREVER       0x02
#define CMD_SAMCONFIGURATION     0x14
#define CMD_INLISTPASSIVETARGET  0x4A
#define CMD_INDATAEXCHANGE       0x40
#define CMD_INRELEASE            0x52

static spi_device_handle_t s_spi;

static inline void cs_low(void)  { gpio_set_level(BOARD_PN532_PIN_CS, 0); }
static inline void cs_high(void) { gpio_set_level(BOARD_PN532_PIN_CS, 1); }

/* Single-byte SPI exchange, manual CS for short status polls */
static uint8_t spi_xfer_byte(uint8_t out)
{
    spi_transaction_t t = {
        .length = 8,
        .tx_buffer = &out,
        .rx_buffer = &out,
        .flags = 0,
    };
    spi_device_polling_transmit(s_spi, &t);
    return out;
}

/* Wait for the PN532 to flag "data ready". Times out in ~50ms. */
static esp_err_t wait_ready(uint32_t timeout_ms)
{
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    while (esp_timer_get_time() < deadline) {
        cs_low();
        spi_xfer_byte(PN532_SPI_STATREAD);
        uint8_t st = spi_xfer_byte(0x00);
        cs_high();
        if (st & 0x01) return ESP_OK;
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    return ESP_ERR_TIMEOUT;
}

/* Build and send a command frame. Body = TFI + command + params. */
static esp_err_t send_frame(const uint8_t *body, size_t body_len)
{
    if (body_len > 254) return ESP_ERR_INVALID_SIZE;

    uint8_t buf[8 + 254];
    size_t i = 0;
    buf[i++] = PN532_SPI_DATAWRITE;
    buf[i++] = PN532_PREAMBLE;
    buf[i++] = PN532_STARTCODE1;
    buf[i++] = PN532_STARTCODE2;
    buf[i++] = (uint8_t)body_len;
    buf[i++] = (uint8_t)(0x100 - body_len);

    uint8_t dcs = 0;
    for (size_t k = 0; k < body_len; k++) {
        buf[i++] = body[k];
        dcs += body[k];
    }
    buf[i++] = (uint8_t)(0x100 - dcs);
    buf[i++] = PN532_POSTAMBLE;

    spi_transaction_t t = {
        .length = i * 8,
        .tx_buffer = buf,
        .rx_buffer = NULL,
    };
    cs_low();
    esp_err_t e = spi_device_polling_transmit(s_spi, &t);
    cs_high();
    return e;
}

/* Read the 6-byte ACK frame: 00 00 FF 00 FF 00 */
static esp_err_t read_ack(void)
{
    if (wait_ready(50) != ESP_OK) return ESP_ERR_TIMEOUT;

    uint8_t exp[] = {0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00};
    uint8_t got[6] = {0};
    cs_low();
    spi_xfer_byte(PN532_SPI_DATAREAD);
    for (int i = 0; i < 6; i++) got[i] = spi_xfer_byte(0x00);
    cs_high();

    if (memcmp(got, exp, 6) != 0) {
        ESP_LOGW(TAG, "bad ACK %02x %02x %02x %02x %02x %02x",
                 got[0], got[1], got[2], got[3], got[4], got[5]);
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

/* Read response frame body into out_body (without preamble/length bytes).
 * Returns body length via out_len, body bytes (TFI..PDn) into out_body. */
static esp_err_t read_response(uint8_t *out_body, size_t buf_len,
                               size_t *out_len, uint32_t timeout_ms)
{
    if (wait_ready(timeout_ms) != ESP_OK) return ESP_ERR_TIMEOUT;

    cs_low();
    spi_xfer_byte(PN532_SPI_DATAREAD);

    uint8_t hdr[5];
    for (int i = 0; i < 5; i++) hdr[i] = spi_xfer_byte(0x00);
    /* hdr = 00 00 FF LEN LCS */
    if (hdr[0] != 0x00 || hdr[1] != 0x00 || hdr[2] != 0xFF) {
        cs_high();
        return ESP_ERR_INVALID_RESPONSE;
    }
    uint8_t len = hdr[3];
    uint8_t lcs = hdr[4];
    if ((uint8_t)(len + lcs) != 0) { cs_high(); return ESP_ERR_INVALID_CRC; }
    if (len > buf_len)             { cs_high(); return ESP_ERR_INVALID_SIZE; }

    uint8_t dcs_calc = 0;
    for (size_t i = 0; i < len; i++) {
        out_body[i] = spi_xfer_byte(0x00);
        dcs_calc += out_body[i];
    }
    uint8_t dcs = spi_xfer_byte(0x00);
    spi_xfer_byte(0x00);   /* postamble */
    cs_high();

    if ((uint8_t)(dcs_calc + dcs) != 0) return ESP_ERR_INVALID_CRC;
    *out_len = len;
    return ESP_OK;
}

/* High-level: send command + read its ACK + read its response. */
static esp_err_t cmd(uint8_t cmd_code, const uint8_t *params, size_t params_len,
                     uint8_t *resp, size_t resp_buf_len, size_t *resp_len)
{
    uint8_t body[1 + 1 + 254];
    body[0] = PN532_HOSTTOPN532;
    body[1] = cmd_code;
    if (params_len) memcpy(&body[2], params, params_len);

    esp_err_t e = send_frame(body, 2 + params_len);
    if (e != ESP_OK) return e;

    e = read_ack();
    if (e != ESP_OK) return e;

    uint8_t r[270];
    size_t rl = 0;
    e = read_response(r, sizeof(r), &rl, 1000);
    if (e != ESP_OK) return e;

    /* r[0] should be PN532TOHOST, r[1] should be cmd_code+1 */
    if (rl < 2 || r[0] != PN532_PN532TOHOST || r[1] != (cmd_code + 1)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    size_t payload = rl - 2;
    if (payload > resp_buf_len) return ESP_ERR_INVALID_SIZE;
    if (payload && resp) memcpy(resp, &r[2], payload);
    if (resp_len) *resp_len = payload;
    return ESP_OK;
}

/* ------- public API ------- */

esp_err_t pn532_init(void)
{
    /* RST + CS as outputs */
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << BOARD_PN532_PIN_CS) | (1ULL << BOARD_PN532_PIN_RST),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&io);
    gpio_set_level(BOARD_PN532_PIN_CS, 1);
    gpio_set_level(BOARD_PN532_PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(BOARD_PN532_PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(50));

    spi_bus_config_t bus = {
        .miso_io_num = BOARD_PN532_PIN_MISO,
        .mosi_io_num = BOARD_PN532_PIN_MOSI,
        .sclk_io_num = BOARD_PN532_PIN_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 512,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(BOARD_PN532_SPI_HOST, &bus, SPI_DMA_CH_AUTO));

    spi_device_interface_config_t dev = {
        .clock_speed_hz = BOARD_PN532_SPI_HZ,
        .mode = 0,
        .spics_io_num = -1,            /* manual CS */
        .queue_size = 4,
        .flags = SPI_DEVICE_BIT_LSBFIRST,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(BOARD_PN532_SPI_HOST, &dev, &s_spi));

    return pn532_wake();
}

esp_err_t pn532_wake(void)
{
    /* SPI wake: pull CS low for ~2ms, send a dummy byte, then GetFirmwareVersion. */
    cs_low();
    vTaskDelay(pdMS_TO_TICKS(2));
    spi_xfer_byte(0x00);
    cs_high();
    vTaskDelay(pdMS_TO_TICKS(2));

    uint8_t resp[16]; size_t rl = 0;
    esp_err_t e = cmd(CMD_GETFIRMWAREVER, NULL, 0, resp, sizeof(resp), &rl);
    if (e == ESP_OK && rl >= 4) {
        ESP_LOGI(TAG, "PN532 IC=0x%02x fw=%u.%u features=0x%02x",
                 resp[0], resp[1], resp[2], resp[3]);
    }
    return e;
}

esp_err_t pn532_sam_config_normal(void)
{
    /* Normal mode, timeout 50ms, no IRQ */
    uint8_t p[] = {0x01, 0x14, 0x00};
    return cmd(CMD_SAMCONFIGURATION, p, sizeof(p), NULL, 0, NULL);
}

esp_err_t pn532_find_target(uint8_t *uid, size_t uid_buf_len,
                            uint8_t *uid_len, uint8_t *sak)
{
    uint8_t p[] = {0x01, 0x00};   /* MaxTg=1, BrTy=ISO14443A */
    uint8_t r[64]; size_t rl = 0;

    esp_err_t e = cmd(CMD_INLISTPASSIVETARGET, p, sizeof(p), r, sizeof(r), &rl);
    if (e != ESP_OK) return e;

    /* Response: NbTg, Tg, SENS_RES(2), SEL_RES, NFCIDLength, NFCID0..n */
    if (rl < 1 || r[0] == 0) return ESP_ERR_NOT_FOUND;
    if (rl < 6) return ESP_ERR_INVALID_RESPONSE;

    uint8_t nfcid_len = r[5];
    if (nfcid_len > uid_buf_len || rl < 6u + nfcid_len) return ESP_ERR_INVALID_SIZE;

    if (sak) *sak = r[4];
    *uid_len = nfcid_len;
    memcpy(uid, &r[6], nfcid_len);
    return ESP_OK;
}

esp_err_t pn532_apdu_exchange(const uint8_t *tx, size_t tx_len,
                              uint8_t *rx, size_t rx_buf_len, size_t *rx_len)
{
    if (tx_len > 252) return ESP_ERR_INVALID_SIZE;

    uint8_t body[1 + 252];
    body[0] = 0x01;                 /* Tg = 1 */
    memcpy(&body[1], tx, tx_len);

    uint8_t r[264]; size_t rl = 0;
    esp_err_t e = cmd(CMD_INDATAEXCHANGE, body, 1 + tx_len, r, sizeof(r), &rl);
    if (e != ESP_OK) return e;

    if (rl < 1) return ESP_ERR_INVALID_RESPONSE;
    /* r[0] is the PN532 status byte; bit 6 set = "more data". For DESFire native
     * commands tunnelled this way, we don't expect chained responses. */
    uint8_t status = r[0] & 0x3F;
    if (status != 0x00) {
        ESP_LOGW(TAG, "InDataExchange status=0x%02x", status);
        return ESP_FAIL;
    }
    size_t pl = rl - 1;
    if (pl > rx_buf_len) return ESP_ERR_INVALID_SIZE;
    memcpy(rx, &r[1], pl);
    *rx_len = pl;
    return ESP_OK;
}

esp_err_t pn532_release(void)
{
    uint8_t p = 0x01;
    return cmd(CMD_INRELEASE, &p, 1, NULL, 0, NULL);
}

/* ------- HAL registration -------
 *
 * Wraps the public pn532_* functions in an nfc_driver_t and registers it
 * with nfc_hal. nfc_init() invokes pn532_init_full() which does SPI bring-up,
 * wake, and SAM config in one shot — matching the HAL contract that init()
 * leaves the chip ready for find_target().
 */

#include "nfc_hal.h"

static esp_err_t pn532_init_full(void)
{
    esp_err_t e = pn532_init();
    if (e != ESP_OK) return e;
    return pn532_sam_config_normal();
}

static const nfc_driver_t pn532_driver = {
    .name          = "pn532",
    .init          = pn532_init_full,
    .find_target   = pn532_find_target,
    .apdu_exchange = pn532_apdu_exchange,
    .release       = pn532_release,
};

void pn532_driver_register(void)
{
    nfc_register_driver(&pn532_driver);
}

#else  /* !CONFIG_NFC_DRIVER_PN532 */

#include "nfc_hal.h"
void pn532_driver_register(void) { /* not selected */ }

#endif /* CONFIG_NFC_DRIVER_PN532 */
