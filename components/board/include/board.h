#pragma once

/*
 * Pin map — ESP32-S3-WROOM-1 (N16R8, 44-pin dev board)
 *
 *  Avoid: GPIO19/20 (USB D-/D+), GPIO26-32 (internal octal flash),
 *         GPIO33-37 (PSRAM), GPIO43/44 (UART0 via USB-UART chip),
 *         GPIO0 (BOOT button / strapping — usable as input after boot),
 *         GPIO3/45/46 (strapping)
 *
 *  PN5180 (SPI, primary NFC):
 *      VCC  -> 5V (TX driver), logic 3.3V
 *      GND  -> GND
 *      SCK  -> GPIO 12  (SPI2 CLK)
 *      MISO -> GPIO 13  (SPI2 MISO)
 *      MOSI -> GPIO 11  (SPI2 MOSI)
 *      NSS  -> GPIO 10  (CS, active low)
 *      BUSY -> GPIO 14  (must be LOW before each command)
 *      RST  -> GPIO 15
 *      IRQ  -> GPIO 16
 *
 *  PN532 (SPI, alternative NFC, DIP1=OFF DIP2=ON):
 *      SCK  -> GPIO 12, MISO -> GPIO 13, MOSI -> GPIO 11
 *      SS   -> GPIO 10, IRQ  -> GPIO 16, RST  -> GPIO  8
 *
 *  HiLetgo TTL<->RS485 (auto flow):
 *      RXD  -> GPIO 17  (UART1 TX from ESP)
 *      TXD  -> GPIO 18  (UART1 RX into ESP)
 *      A/B  -> OSDP bus
 *
 *  Indicators / IO:
 *      On-board WS2812B RGB LED   -> GPIO 48 (no external hardware needed)
 *      Active piezo (HIGH=on)     -> GPIO 21
 *      Mode button (BOOT button)  -> GPIO  0, active-low, internal pull-up
 *
 *  Debug console: UART0 via on-board USB-UART chip (GPIO43/44) — unchanged
 *
 *  Decoupling: put 10µF + 100nF at PN5180 TVDD pin to prevent RF dropouts
 *  during DESFire writes.
 */

/* PN532 SPI */
#define BOARD_PN532_SPI_HOST    SPI2_HOST
#define BOARD_PN532_PIN_SCK     12
#define BOARD_PN532_PIN_MISO    13
#define BOARD_PN532_PIN_MOSI    11
#define BOARD_PN532_PIN_CS      10
#define BOARD_PN532_PIN_IRQ     16
#define BOARD_PN532_PIN_RST      8
#define BOARD_PN532_SPI_HZ      (5 * 1000 * 1000)

/* PN5180 SPI */
#define BOARD_PN5180_SPI_HOST    SPI2_HOST
#define BOARD_PN5180_PIN_SCK     12
#define BOARD_PN5180_PIN_MISO    13
#define BOARD_PN5180_PIN_MOSI    11
#define BOARD_PN5180_PIN_NSS     10
#define BOARD_PN5180_PIN_BUSY    14
#define BOARD_PN5180_PIN_RST     15
#define BOARD_PN5180_PIN_IRQ     16
#define BOARD_PN5180_SPI_HZ     (7 * 1000 * 1000)

/* RS485 / OSDP */
#define BOARD_OSDP_UART_NUM      UART_NUM_1
#define BOARD_OSDP_PIN_TX        17
#define BOARD_OSDP_PIN_RX        18
#define BOARD_OSDP_BAUD          9600

/* Indicators — on-board WS2812B RGB LED replaces discrete green/red LEDs */
#define BOARD_PIN_RGB_LED        48
#define BOARD_PIN_BUZZER         21
#define BOARD_PIN_BUTTON          0   /* BOOT button, active-low */

/* OSDP identity */
#define BOARD_OSDP_PD_ADDRESS    0x65
