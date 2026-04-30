#include "desfire.h"
#include "desfire_internal.h"
#include "nfc_hal.h"
#include "esp_log.h"

#include <string.h>

static const char *TAG = "desfire";
static uint8_t s_last_status = DF_OPERATION_OK;

void desfire_init(void) { memset(&g_session, 0, sizeof(g_session)); }
uint8_t desfire_last_status(void) { return s_last_status; }

/* Native DESFire commands run as ISO-wrapped APDUs:
 *   CLA=0x90, INS=cmd, P1=0, P2=0, Lc=len(params), params, Le=0
 * Response is data || SW1 SW2, where SW1=0x91 SW2=DESFire-status.
 */
int desfire_xfer(uint8_t cmd, const uint8_t *params, size_t params_len,
                 uint8_t *resp_data, size_t resp_buf, size_t *resp_len)
{
    uint8_t apdu[5 + 256 + 1];
    if (params_len > 255) return -1;

    size_t i = 0;
    apdu[i++] = 0x90;
    apdu[i++] = cmd;
    apdu[i++] = 0x00;
    apdu[i++] = 0x00;
    apdu[i++] = (uint8_t)params_len;
    if (params_len) { memcpy(&apdu[i], params, params_len); i += params_len; }
    apdu[i++] = 0x00;   /* Le */

    uint8_t rx[280];
    size_t rxl = 0;
    esp_err_t e = nfc_apdu_exchange(apdu, i, rx, sizeof(rx), &rxl);
    if (e != ESP_OK || rxl < 2) {
        s_last_status = 0xFF;
        return -1;
    }
    if (rx[rxl - 2] != 0x91) {
        ESP_LOGW(TAG, "unexpected SW1=0x%02x", rx[rxl - 2]);
        s_last_status = 0xFF;
        return -1;
    }
    s_last_status = rx[rxl - 1];

    size_t data_len = rxl - 2;
    if (data_len > resp_buf) return -1;
    if (data_len && resp_data) memcpy(resp_data, rx, data_len);
    if (resp_len) *resp_len = data_len;
    return s_last_status;
}

/* ------- post-auth wrap/unwrap helpers -------
 *
 * After AuthenticateEV2First, every command needs:
 *   - cmd_ctr is incremented (host-side)
 *   - CMAC mode: append truncated-MAC(8) over [cmd | cmd_ctr_le | TI | cmd_header | cmd_data]
 *     to the plaintext command data.
 *   - ENC mode: encrypt [cmd_data | crc32(cmd | cmd_header | cmd_data)] with K_ses_enc
 *     and IV derived from cmd_ctr/TI, then CMAC over the ciphertext. Implemented in
 *     desfire_enc.c (desfire_enc_send / desfire_enc_recv / desfire_change_key).
 */

static esp_err_t cmac_append_and_send(uint8_t cmd, const uint8_t *header, size_t hlen,
                                      const uint8_t *data, size_t dlen,
                                      uint8_t *resp, size_t resp_buf, size_t *resp_len)
{
    /* Build MAC input: cmd | cmd_ctr_le | TI | header | data */
    uint8_t macbuf[1 + 2 + 4 + 256];
    if (hlen + dlen > 256) return ESP_ERR_INVALID_SIZE;

    size_t mi = 0;
    macbuf[mi++] = cmd;
    macbuf[mi++] = (uint8_t)(g_session.cmd_ctr & 0xFF);
    macbuf[mi++] = (uint8_t)(g_session.cmd_ctr >> 8);
    memcpy(&macbuf[mi], g_session.ti, 4); mi += 4;
    if (hlen) { memcpy(&macbuf[mi], header, hlen); mi += hlen; }
    if (dlen) { memcpy(&macbuf[mi], data,   dlen); mi += dlen; }

    uint8_t cmac_full[16];
    desfire_cmac(macbuf, mi, cmac_full);

    /* Even-indexed bytes of full CMAC = on-wire 8-byte MAC */
    uint8_t mac8[8];
    for (int i = 0; i < 8; i++) mac8[i] = cmac_full[1 + 2 * i];

    /* Params = header | data | mac8 */
    uint8_t params[256];
    size_t pl = 0;
    if (hlen) { memcpy(&params[pl], header, hlen); pl += hlen; }
    if (dlen) { memcpy(&params[pl], data,   dlen); pl += dlen; }
    memcpy(&params[pl], mac8, 8); pl += 8;

    int st = desfire_xfer(cmd, params, pl, resp, resp_buf, resp_len);
    if (st < 0) return ESP_FAIL;
    if (st != DF_OPERATION_OK && st != DF_ADDITIONAL_FRAME) return ESP_FAIL;

    g_session.cmd_ctr++;

    /* Verify response MAC: response data ends with 8-byte MAC */
    if (resp_len && *resp_len >= 8) {
        size_t rd = *resp_len - 8;
        uint8_t verify[256];
        size_t vi = 0;
        verify[vi++] = (uint8_t)st;
        verify[vi++] = (uint8_t)(g_session.cmd_ctr & 0xFF);
        verify[vi++] = (uint8_t)(g_session.cmd_ctr >> 8);
        memcpy(&verify[vi], g_session.ti, 4); vi += 4;
        if (rd) { memcpy(&verify[vi], resp, rd); vi += rd; }

        uint8_t want_full[16], want8[8];
        desfire_cmac(verify, vi, want_full);
        for (int i = 0; i < 8; i++) want8[i] = want_full[1 + 2 * i];

        if (memcmp(&resp[rd], want8, 8) != 0) {
            ESP_LOGE(TAG, "response CMAC mismatch");
            return ESP_ERR_INVALID_CRC;
        }
        *resp_len = rd;
    }

    return ESP_OK;
}

/* ------- public ops ------- */

esp_err_t desfire_select_application(const uint8_t aid[3])
{
    /* SelectApplication is plain; resets auth state on the card. */
    int st = desfire_xfer(DF_CMD_SELECT_APP, aid, 3, NULL, 0, NULL);
    if (st < 0 || st != DF_OPERATION_OK) return ESP_FAIL;
    g_session.authenticated = false;
    g_session.cmd_ctr = 0;
    return ESP_OK;
}

esp_err_t desfire_get_uid(uint8_t uid[7])
{
    if (!g_session.authenticated) return ESP_ERR_INVALID_STATE;
    /* GetCardUID returns 7 bytes encrypted with K_ses_enc + CRC32 inside.
     * No header or command params — just the cmd byte itself. */
    uint8_t buf[7];
    size_t  rl = 0;
    int st = desfire_enc_recv(DF_CMD_GET_UID, NULL, 0, 7, buf, sizeof(buf), &rl);
    if (st != DF_OPERATION_OK || rl != 7) return ESP_FAIL;
    memcpy(uid, buf, 7);
    return ESP_OK;
}

esp_err_t desfire_format_picc(void)
{
    if (!g_session.authenticated) return ESP_ERR_INVALID_STATE;
    return cmac_append_and_send(DF_CMD_FORMAT_PICC, NULL, 0, NULL, 0, NULL, 0, NULL);
}

esp_err_t desfire_create_application(const uint8_t aid[3],
                                     uint8_t key_settings,
                                     uint8_t num_keys_and_flags)
{
    /* AID(3) | KeySettings(1) | KeyTypeAndCount(1)
     * num_keys_and_flags upper nibble = key type (0x80 = AES), lower nibble = count
     */
    uint8_t hdr[5];
    memcpy(&hdr[0], aid, 3);
    hdr[3] = key_settings;
    hdr[4] = num_keys_and_flags;

    if (g_session.authenticated) {
        return cmac_append_and_send(DF_CMD_CREATE_APP, hdr, sizeof(hdr),
                                    NULL, 0, NULL, 0, NULL);
    }
    int st = desfire_xfer(DF_CMD_CREATE_APP, hdr, sizeof(hdr), NULL, 0, NULL);
    return (st == DF_OPERATION_OK) ? ESP_OK : ESP_FAIL;
}

esp_err_t desfire_create_std_data_file(uint8_t file_no,
                                       desfire_comm_mode_t comm,
                                       uint16_t access_rights,
                                       uint32_t file_size)
{
    uint8_t hdr[7];
    hdr[0] = file_no;
    hdr[1] = (uint8_t)comm;
    hdr[2] = (uint8_t)(access_rights & 0xFF);
    hdr[3] = (uint8_t)(access_rights >> 8);
    hdr[4] = (uint8_t)(file_size & 0xFF);
    hdr[5] = (uint8_t)((file_size >> 8) & 0xFF);
    hdr[6] = (uint8_t)((file_size >> 16) & 0xFF);

    if (!g_session.authenticated) return ESP_ERR_INVALID_STATE;
    return cmac_append_and_send(DF_CMD_CREATE_STD_FILE, hdr, sizeof(hdr),
                                NULL, 0, NULL, 0, NULL);
}

/* desfire_change_key() is implemented in desfire_enc.c (encrypted-mode wrapper) */

esp_err_t desfire_read_data(uint8_t file_no,
                            uint32_t offset, uint32_t length,
                            desfire_comm_mode_t comm,
                            uint8_t *out, size_t out_buf, size_t *out_len)
{
    uint8_t hdr[7];
    hdr[0] = file_no;
    hdr[1] = (uint8_t)(offset & 0xFF);
    hdr[2] = (uint8_t)((offset >> 8) & 0xFF);
    hdr[3] = (uint8_t)((offset >> 16) & 0xFF);
    hdr[4] = (uint8_t)(length & 0xFF);
    hdr[5] = (uint8_t)((length >> 8) & 0xFF);
    hdr[6] = (uint8_t)((length >> 16) & 0xFF);

    if (comm == DESFIRE_COMM_PLAIN) {
        int st = desfire_xfer(DF_CMD_READ_DATA, hdr, sizeof(hdr),
                              out, out_buf, out_len);
        return (st == DF_OPERATION_OK) ? ESP_OK : ESP_FAIL;
    }
    if (comm == DESFIRE_COMM_CMAC) {
        if (!g_session.authenticated) return ESP_ERR_INVALID_STATE;
        return cmac_append_and_send(DF_CMD_READ_DATA, hdr, sizeof(hdr),
                                    NULL, 0, out, out_buf, out_len);
    }
    if (comm == DESFIRE_COMM_ENCRYPTED) {
        if (!g_session.authenticated) return ESP_ERR_INVALID_STATE;
        if (length == 0 || length > out_buf) return ESP_ERR_INVALID_SIZE;
        int st = desfire_enc_recv(DF_CMD_READ_DATA, hdr, sizeof(hdr),
                                  length, out, out_buf, out_len);
        return (st == DF_OPERATION_OK) ? ESP_OK : ESP_FAIL;
    }
    return ESP_FAIL;
}

esp_err_t desfire_write_data(uint8_t file_no,
                             uint32_t offset, uint32_t length,
                             desfire_comm_mode_t comm,
                             const uint8_t *data)
{
    uint8_t hdr[7];
    hdr[0] = file_no;
    hdr[1] = (uint8_t)(offset & 0xFF);
    hdr[2] = (uint8_t)((offset >> 8) & 0xFF);
    hdr[3] = (uint8_t)((offset >> 16) & 0xFF);
    hdr[4] = (uint8_t)(length & 0xFF);
    hdr[5] = (uint8_t)((length >> 8) & 0xFF);
    hdr[6] = (uint8_t)((length >> 16) & 0xFF);

    if (comm == DESFIRE_COMM_PLAIN) {
        uint8_t buf[256];
        if (sizeof(hdr) + length > sizeof(buf)) return ESP_ERR_INVALID_SIZE;
        memcpy(&buf[0], hdr, sizeof(hdr));
        memcpy(&buf[sizeof(hdr)], data, length);
        int st = desfire_xfer(DF_CMD_WRITE_DATA, buf, sizeof(hdr) + length, NULL, 0, NULL);
        return (st == DF_OPERATION_OK) ? ESP_OK : ESP_FAIL;
    }
    if (comm == DESFIRE_COMM_CMAC) {
        if (!g_session.authenticated) return ESP_ERR_INVALID_STATE;
        return cmac_append_and_send(DF_CMD_WRITE_DATA, hdr, sizeof(hdr),
                                    data, length, NULL, 0, NULL);
    }
    if (comm == DESFIRE_COMM_ENCRYPTED) {
        if (!g_session.authenticated) return ESP_ERR_INVALID_STATE;
        int st = desfire_enc_send(DF_CMD_WRITE_DATA, hdr, sizeof(hdr),
                                  data, length, NULL, 0, NULL);
        return (st == DF_OPERATION_OK) ? ESP_OK : ESP_FAIL;
    }
    return ESP_FAIL;
}
