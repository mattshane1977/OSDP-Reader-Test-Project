#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "board_io.h"
#include "leaf.h"

/* Events the NFC task pushes up; the OSDP task and any UI consume them. */
typedef enum {
    EVT_CARD_READ_OK,
    EVT_CARD_READ_FAIL,
    EVT_CARD_WRITE_OK,
    EVT_CARD_WRITE_FAIL,
    EVT_MODE_CHANGED,
} reader_event_kind_t;

typedef struct {
    reader_event_kind_t kind;
    reader_mode_t       mode;          /* current mode at time of event */
    leaf_credential_t   cred;          /* valid for CARD_READ_OK and WRITE_OK */
    uint8_t             uid[7];
    uint8_t             uid_len;
} reader_event_t;

/* Pre-loaded credential to write next time a card lands in the field while
 * mode == WRITE. The UI task sets this. */
typedef struct {
    leaf_credential_t cred;
    bool              pending;
} reader_write_request_t;

void mode_init(void);
reader_mode_t mode_get(void);
void          mode_set(reader_mode_t m);
void          mode_handle_button(button_event_t btn);

/* Multi-subscriber event bus.
 *
 * Replaces the old single-queue model. Each consumer (osdp_pd, console,
 * webui) calls mode_subscribe() once during startup to get its own queue
 * handle. mode_post_event() fans out to all live subscribers via xQueueSend
 * with zero timeout — a slow consumer drops events rather than blocking the
 * NFC task.
 *
 * Maximum subscribers: 4. Subscribing more will return NULL.
 */
QueueHandle_t mode_subscribe(const char *name);
void          mode_post_event(const reader_event_t *e);

/* Legacy single-queue accessor — retained for backwards compat. Returns the
 * first subscriber registered. Kept so existing osdp_pd code still compiles
 * during the transition. New code should use mode_subscribe(). */
QueueHandle_t mode_event_queue(void);

/* UI / OSDP-side write request setter */
void mode_set_write_request(const leaf_credential_t *c);
bool mode_take_write_request(leaf_credential_t *out);
