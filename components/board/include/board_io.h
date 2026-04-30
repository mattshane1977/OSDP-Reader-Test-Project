#pragma once

#include <stdint.h>
#include <stdbool.h>

/* Reader operating modes — drives both behaviour and indicator patterns */
typedef enum {
    READER_MODE_IDLE = 0,
    READER_MODE_READ,        /* normal access-control read */
    READER_MODE_WRITE,       /* personalization / enroll */
} reader_mode_t;

/* Indicator patterns — async, non-blocking; latest call wins */
typedef enum {
    IND_OFF = 0,
    IND_IDLE,                /* dim heartbeat — slow green pulse */
    IND_ARMED_READ,          /* solid green, ready */
    IND_ARMED_WRITE,         /* slow red blink, ready to enroll */
    IND_GRANT,               /* short green + single beep */
    IND_DENY,                /* red flash + double beep */
    IND_WROTE_OK,            /* green + red alternate, triple beep */
    IND_ERROR,               /* fast red blink + long beep */
} indicator_pattern_t;

void board_init(void);

/* Direct LED / buzzer control — used by libosdp LED+buzzer commands */
void board_led_set(bool green_on, bool red_on);
void board_buzzer_set(bool on);

/* High-level patterns — drives an internal task */
void board_indicate(indicator_pattern_t p);

/* Mode button — returns the latched edge events; non-blocking */
typedef enum {
    BTN_NONE = 0,
    BTN_SHORT,               /* < 1s press */
    BTN_LONG,                /* >= 2s press */
    BTN_FACTORY_RESET,       /* >= 10s press */
} button_event_t;

button_event_t board_button_poll(void);
