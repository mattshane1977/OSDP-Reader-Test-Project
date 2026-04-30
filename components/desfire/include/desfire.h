#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

/*
 * DESFire EV3 driver — minimum surface needed for a LEAF-style credential.
 *
 * What's implemented:
 *   - ISOSelect AID (native command 0x5A)
 *   - AuthenticateEV2First (0x71) — AES-128 mutual auth with session keys
 *   - CMAC verification on every subsequent response
 *   - ReadData (0xBD) plain / CMAC / encrypted
 *   - WriteData (0x3D) plain / CMAC / encrypted
 *   - CreateApplication, CreateStdDataFile, ChangeKey for personalization
 *   - Format PICC (factory reset to default master key — handy for dev)
 *
 * What's not (yet):
 *   - AuthenticateEV2NonFirst (only matters if you re-auth without selecting again)
 *   - Transaction MAC files (EV3-specific; LEAF doesn't need them for a static credential)
 *   - Original Authenticate(0x0A) / AuthenticateISO(0x1A) — EV3 supports them but
 *     EV2First is the secure path.
 *
 * All functions return ESP_OK on success. On a DESFire status code other than
 * OPERATION_OK / ADDITIONAL_FRAME, ESP_FAIL is returned and the status byte is
 * available through desfire_last_status().
 */

#define DESFIRE_AES_KEY_LEN     16
#define DESFIRE_AID_LEN         3
#define DESFIRE_MAX_FILE_SIZE   8192   /* practical cap for our reads */

typedef enum {
    DESFIRE_COMM_PLAIN     = 0,
    DESFIRE_COMM_CMAC      = 1,
    DESFIRE_COMM_ENCRYPTED = 3,
} desfire_comm_mode_t;

typedef struct {
    uint8_t  key_no;                    /* which key auth'd */
    uint8_t  ti[4];                     /* transaction identifier */
    uint8_t  k_ses_auth_enc[16];        /* session enc key */
    uint8_t  k_ses_auth_mac[16];        /* session mac key */
    uint16_t cmd_ctr;                   /* increments per command after auth */
    bool     authenticated;
} desfire_session_t;

void desfire_init(void);
uint8_t desfire_last_status(void);

/* Card lifecycle */
esp_err_t desfire_select_application(const uint8_t aid[DESFIRE_AID_LEN]);
esp_err_t desfire_format_picc(void);            /* requires master-key auth on PICC level */
esp_err_t desfire_get_uid(uint8_t uid[7]);      /* the real UID, even when random ID is on */

/* Auth */
esp_err_t desfire_auth_ev2_first(uint8_t key_no,
                                 const uint8_t key[DESFIRE_AES_KEY_LEN]);

/* PICC-level personalization */
esp_err_t desfire_create_application(const uint8_t aid[DESFIRE_AID_LEN],
                                     uint8_t key_settings,
                                     uint8_t num_keys_and_flags);

/* Application-level personalization */
esp_err_t desfire_create_std_data_file(uint8_t file_no,
                                       desfire_comm_mode_t comm,
                                       uint16_t access_rights,
                                       uint32_t file_size);

esp_err_t desfire_change_key(uint8_t key_no,
                             const uint8_t old_key[DESFIRE_AES_KEY_LEN],
                             const uint8_t new_key[DESFIRE_AES_KEY_LEN],
                             uint8_t key_version);

/* Data ops */
esp_err_t desfire_read_data(uint8_t file_no,
                            uint32_t offset, uint32_t length,
                            desfire_comm_mode_t comm,
                            uint8_t *out, size_t out_buf, size_t *out_len);

esp_err_t desfire_write_data(uint8_t file_no,
                             uint32_t offset, uint32_t length,
                             desfire_comm_mode_t comm,
                             const uint8_t *data);
