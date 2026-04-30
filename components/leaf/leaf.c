#include "leaf.h"
#include "desfire.h"
#include "desfire_internal.h"

#include "nvs_flash.h"
#include "nvs.h"
#include "mbedtls/aes.h"
#include "esp_log.h"

#include <string.h>

static const char *TAG = "leaf";
static const char *NVS_NS  = "leaf";
static const char *NVS_KEY = "site_key";

static const uint8_t LEAF_AID[3]      = { LEAF_AID_BYTE0, LEAF_AID_BYTE1, LEAF_AID_BYTE2 };
static const uint8_t PICC_AID[3]      = { 0x00, 0x00, 0x00 };
static const uint8_t DEFAULT_KEY[16]  = { 0 };   /* factory default */

/* ---- NVS site key ---- */

void leaf_init(void)
{
    /* nvs_flash_init() is expected to be called from app_main */
}

esp_err_t leaf_set_site_key(const uint8_t key[16])
{
    nvs_handle_t h;
    esp_err_t e = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    e = nvs_set_blob(h, NVS_KEY, key, 16);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e;
}

esp_err_t leaf_get_site_key(uint8_t key[16])
{
    nvs_handle_t h;
    esp_err_t e = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (e != ESP_OK) return e;
    size_t sz = 16;
    e = nvs_get_blob(h, NVS_KEY, key, &sz);
    nvs_close(h);
    if (e != ESP_OK || sz != 16) return ESP_ERR_NVS_NOT_FOUND;
    return ESP_OK;
}

/* ---- AN10922 AES-128 diversification ---- */

void leaf_diversify_key(const uint8_t site_key[16],
                          const uint8_t *uid, size_t uid_len,
                          uint8_t out_key[16])
{
    /* Input M = 0x01 || UID || AID || pad
     * Pad to a multiple of 16 with 0x80, 0x00... if needed (NIST CMAC padding
     * happens inside CMAC; AN10922 just specifies the diversification input).
     *
     * For our case: 1 + 7 (UID) + 3 (AID) = 11 bytes — CMAC's own padding
     * handles it.
     */
    uint8_t m[1 + 16];
    size_t mi = 0;
    m[mi++] = 0x01;
    if (uid_len > 7) uid_len = 7;
    memcpy(&m[mi], uid, uid_len); mi += uid_len;
    memcpy(&m[mi], LEAF_AID, 3);  mi += 3;

    /* Use CMAC with site_key */
    extern uint8_t g_session_mac_key_full[16];
    uint8_t saved[16];
    memcpy(saved, g_session_mac_key_full, 16);
    memcpy(g_session_mac_key_full, site_key, 16);
    desfire_cmac(m, mi, out_key);
    memcpy(g_session_mac_key_full, saved, 16);
}

/* ---- credential payload helpers ---- */

void leaf_pack_credential(const leaf_credential_t *c, uint8_t out[10])
{
    out[0] = (uint8_t)(c->facility & 0xFF);
    out[1] = (uint8_t)(c->facility >> 8);
    out[2] = (uint8_t)(c->card_id & 0xFF);
    out[3] = (uint8_t)((c->card_id >> 8) & 0xFF);
    out[4] = (uint8_t)((c->card_id >> 16) & 0xFF);
    out[5] = (uint8_t)((c->card_id >> 24) & 0xFF);
    out[6] = (uint8_t)(c->issue_date & 0xFF);
    out[7] = (uint8_t)((c->issue_date >> 8) & 0xFF);
    out[8] = (uint8_t)((c->issue_date >> 16) & 0xFF);
    out[9] = (uint8_t)((c->issue_date >> 24) & 0xFF);
}

void leaf_unpack_credential(const uint8_t in[10], leaf_credential_t *c)
{
    c->facility   = (uint16_t)(in[0] | (in[1] << 8));
    c->card_id    = (uint32_t)(in[2] | (in[3] << 8) | (in[4] << 16) | (in[5] << 24));
    c->issue_date = (uint32_t)(in[6] | (in[7] << 8) | (in[8] << 16) | (in[9] << 24));
}

void leaf_payload_mac(const uint8_t div_key[16], const uint8_t payload[10], uint8_t mac8[8])
{
    extern uint8_t g_session_mac_key_full[16];
    uint8_t saved[16];
    memcpy(saved, g_session_mac_key_full, 16);
    memcpy(g_session_mac_key_full, div_key, 16);
    uint8_t full[16];
    desfire_cmac(payload, 10, full);
    memcpy(g_session_mac_key_full, saved, 16);
    for (int i = 0; i < 8; i++) mac8[i] = full[1 + 2 * i];
}

/* ---- read flow ---- */

esp_err_t leaf_read(const uint8_t *uid, size_t uid_len, leaf_credential_t *out)
{
    uint8_t site[16];
    if (leaf_get_site_key(site) != ESP_OK) {
        ESP_LOGE(TAG, "no site key in NVS — provision one before reading");
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t k_div[16];
    leaf_diversify_key(site, uid, uid_len, k_div);

    if (desfire_select_application(LEAF_AID) != ESP_OK) return ESP_FAIL;


    /* Auth with key 1 (read key, diversified) */
    if (desfire_auth_ev2_first(1, k_div) != ESP_OK) return ESP_FAIL;

    uint8_t buf[LEAF_FILE_SIZE];
    size_t  rl = 0;
    esp_err_t e = desfire_read_data(LEAF_FILE_NO, 0, LEAF_FILE_SIZE,
                                    DESFIRE_COMM_CMAC, buf, sizeof(buf), &rl);
    if (e != ESP_OK || rl != LEAF_FILE_SIZE) {
        ESP_LOGW(TAG, "read failed: e=%d rl=%u", e, (unsigned)rl);
        return ESP_FAIL;
    }

    /* Verify the embedded payload MAC */
    uint8_t want[8];
    leaf_payload_mac(k_div, buf, want);
    if (memcmp(want, &buf[10], 8) != 0) {
        ESP_LOGE(TAG, "credential MAC mismatch");
        return ESP_ERR_INVALID_CRC;
    }
    leaf_unpack_credential(buf, out);
    return ESP_OK;
}

/* ---- personalize flow ---- */

esp_err_t leaf_personalize(const uint8_t *uid, size_t uid_len,
                           const leaf_credential_t *cred)
{
    uint8_t site[16];
    if (leaf_get_site_key(site) != ESP_OK) {
        ESP_LOGE(TAG, "no site key — call leaf_set_site_key first");
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t k_div[16];
    leaf_diversify_key(site, uid, uid_len, k_div);

    /* Step 1: select PICC, auth with default master */
    if (desfire_select_application(PICC_AID) != ESP_OK) return ESP_FAIL;
    if (desfire_auth_ev2_first(0, DEFAULT_KEY) != ESP_OK) {
        ESP_LOGW(TAG, "PICC default-key auth failed — card may already be personalized");
        return ESP_FAIL;
    }

    /* Step 2: format (clean slate). Optional but good for re-personalization. */
    if (desfire_format_picc() != ESP_OK) {
        ESP_LOGW(TAG, "format_picc failed (status=0x%02x)", desfire_last_status());
        /* not fatal if the card is already empty */
    }

    /* Step 3: create application — 2 AES keys, key settings 0x0F (anyone can list) */
    /* num_keys_and_flags: 0x80 = AES, low nibble = key count (2) */
    if (desfire_create_application(LEAF_AID, 0x0F, 0x82) != ESP_OK) {
        ESP_LOGE(TAG, "create_app failed (status=0x%02x)", desfire_last_status());
        return ESP_FAIL;
    }

    /* Step 4: select our app, auth with default key 0 */
    if (desfire_select_application(LEAF_AID) != ESP_OK) return ESP_FAIL;
    if (desfire_auth_ev2_first(0, DEFAULT_KEY) != ESP_OK) return ESP_FAIL;

    /* Step 5: create credential file
     *   access_rights: 0x1010 — read=key1, write=key0, read&write=key0, change=key0
     *   nibbles (high to low): change | r&w | write | read = 0 0 0 1 -> 0x0001 ?
     *
     * DESFire encodes access_rights as a 16-bit value with 4 nibbles:
     *   bits 15..12 = ReadKey
     *   bits 11..8  = WriteKey
     *   bits  7..4  = ReadWriteKey
     *   bits  3..0  = ChangeKey
     * 0xE means "free" (no auth needed), 0xF means "deny".
     *
     * We want: ReadKey=1, WriteKey=0, ReadWriteKey=0, ChangeKey=0  -> 0x1000
     */
    if (desfire_create_std_data_file(LEAF_FILE_NO, DESFIRE_COMM_CMAC,
                                     0x1000, LEAF_FILE_SIZE) != ESP_OK) {
        ESP_LOGE(TAG, "create_file failed (status=0x%02x)", desfire_last_status());
        return ESP_FAIL;
    }

    /* Step 6: change keys 1 (read) and 0 (app master) to their diversified
     * versions. Order matters:
     *   - Change key 1 first, while still auth'd to key 0 (different-key branch).
     *   - Change key 0 last, since changing the auth'd key invalidates the session.
     */
    uint8_t k_div_master[16];
    {
        /* derive a *second* key with a different prefix (0x02) so master != read key */
        uint8_t m[1 + 16];
        size_t mi = 0;
        m[mi++] = 0x02;
        if (uid_len > 7) uid_len = 7;
        memcpy(&m[mi], uid, uid_len); mi += uid_len;
        memcpy(&m[mi], LEAF_AID, 3);  mi += 3;

        extern uint8_t g_session_mac_key_full[16];
        uint8_t saved[16];
        memcpy(saved, g_session_mac_key_full, 16);
        memcpy(g_session_mac_key_full, site, 16);
        desfire_cmac(m, mi, k_div_master);
        memcpy(g_session_mac_key_full, saved, 16);
    }

    /* Different-key branch: rotate read key (1) while auth'd as key 0 */
    if (desfire_change_key(1, DEFAULT_KEY, k_div, 0x01) != ESP_OK) {
        ESP_LOGE(TAG, "ChangeKey(1) failed");
        return ESP_FAIL;
    }

    /* Same-key branch: rotate the auth'd master (0). Session ends after this. */
    if (desfire_change_key(0, DEFAULT_KEY, k_div_master, 0x01) != ESP_OK) {
        ESP_LOGE(TAG, "ChangeKey(0) failed");
        return ESP_FAIL;
    }

    /* Step 7: re-auth as the new diversified master, then write credential. */
    if (desfire_auth_ev2_first(0, k_div_master) != ESP_OK) {
        ESP_LOGE(TAG, "post-rotation auth failed");
        return ESP_FAIL;
    }

    uint8_t payload[LEAF_FILE_SIZE];
    leaf_pack_credential(cred, payload);
    leaf_payload_mac(k_div, payload, &payload[10]);

    return desfire_write_data(LEAF_FILE_NO, 0, LEAF_FILE_SIZE,
                              DESFIRE_COMM_CMAC, payload);
}
