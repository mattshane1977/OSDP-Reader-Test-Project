#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

/*
 * "LEAF-style" credential layout for blank EV3 cards.
 *
 * This is a clean-room layout that mirrors the *shape* of a real LEAF
 * credential without using any LEAF-proprietary AIDs, key-derivation, or file
 * structure. You'll change all of these constants for production use; they
 * exist so the read and write paths agree.
 *
 *   AID:                    0xF4 0xEA 0x00          (placeholder)
 *   App keys:               2 AES-128 (key 0 = master, key 1 = read key)
 *   File 0x01 (creds):      Standard data file, 16 bytes, CMAC mode
 *                           access rights: read=key1, write=key0,
 *                                          read&write=key0, change=key0
 *   File contents (16 B):   facility(2) | card_id(4) | issue_date(4) |
 *                            mac8(8 — CMAC over first 10 bytes with diversified key)
 *
 * Per-card key diversification (NXP AN10922-style, AES-128):
 *   K_div = AES-CMAC(K_master, 0x01 || UID(7) || AID(3) || 0x00..0x00 padding to 16)
 *
 *   - K_master is your site key (stored in encrypted NVS)
 *   - K_div is what gets written to the card as key 1 during personalization
 *   - The reader recomputes K_div from the card's UID before authenticating
 *
 * This means dumping one card never reveals the site key, and re-keying the
 * fleet means rotating one secret in NVS rather than touching every card.
 */

#define LEAF_AID_BYTE0     0xF4
#define LEAF_AID_BYTE1     0xEA
#define LEAF_AID_BYTE2     0x00

#define LEAF_FILE_NO       0x01
#define LEAF_FILE_SIZE     32

typedef struct {
    uint16_t facility;
    uint32_t card_id;
    uint32_t issue_date;     /* days since some epoch — your call */
} leaf_credential_t;

void leaf_init(void);

/* Set/replace the site master key. Persists in encrypted NVS. */
esp_err_t leaf_set_site_key(const uint8_t key[16]);
esp_err_t leaf_get_site_key(uint8_t key[16]);

/* Read flow: select app, derive K_div from UID, auth key 1, read file 1, verify MAC. */
esp_err_t leaf_read(const uint8_t *uid, size_t uid_len, leaf_credential_t *out);

/* Write flow (personalization): assumes card is at factory defaults
 *   (PICC master = 16×0x00, AES). Steps:
 *      1. Select PICC (AID 0x00 0x00 0x00), auth key 0 with default key
 *      2. Format PICC (clean slate)
 *      3. Create application with our AID and 2 AES keys
 *      4. Select our application, auth key 0 with default
 *      5. Create the credential file
 *      6. ChangeKey(0) -> diversified app master, ChangeKey(1) -> diversified read key
 *      7. Re-auth key 0, write the credential payload + CMAC
 *
 * Returns ESP_OK on success.
 */
esp_err_t leaf_personalize(const uint8_t *uid, size_t uid_len,
                           const leaf_credential_t *cred);

/* Public for unit testing */
void leaf_pack_credential(const leaf_credential_t *c, uint8_t out[10]);
void leaf_unpack_credential(const uint8_t in[10], leaf_credential_t *c);
void leaf_payload_mac(const uint8_t div_key[16], const uint8_t payload[10], uint8_t mac8[8]);
void leaf_diversify_key(const uint8_t site_key[16], const uint8_t *uid, size_t uid_len, uint8_t out_key[16]);
