#pragma once

/*
 * Pin map — ESP-WROOM-32
 *
 *  PN532 (SPI mode, DIP1=OFF DIP2=ON):
 *      VCC  -> 3V3
 *      GND  -> GND
 *      SCK  -> GPIO 18  (VSPI CLK)
 *      MISO -> GPIO 19  (VSPI MISO)
 *      MOSI -> GPIO 23  (VSPI MOSI)
 *      SS   -> GPIO  5  (CS, active low)
 *      IRQ  -> GPIO  4  (optional, used for InListPassive polling wake)
 *      RST  -> GPIO  2
 *
 *  HiLetgo TTL<->RS485 (auto flow):
 *      VCC  -> 5V (board has 3V3 logic level shifter; data lines are 3V3 safe)
 *      GND  -> GND
 *      RXD  -> GPIO 17  (UART2 TX from ESP)
 *      TXD  -> GPIO 16  (UART2 RX into ESP)
 *      A/B  -> OSDP bus
 *
 *  Indicators / IO:
 *      Green LED (active high) -> GPIO 25, series resistor to GND
 *      Red   LED (active high) -> GPIO 26
 *      Active piezo (HIGH=on)  -> GPIO 27
 *      Mode button (to GND)    -> GPIO 32, internal pull-up
 *
 *  Reserved: GPIOs 6-11 (flash), 0/12/15 (strapping — avoid for outputs at boot)
 */

/* PN532 SPI */
#define BOARD_PN532_SPI_HOST    VSPI_HOST
#define BOARD_PN532_PIN_SCK     18
#define BOARD_PN532_PIN_MISO    19
#define BOARD_PN532_PIN_MOSI    23
#define BOARD_PN532_PIN_CS       5
#define BOARD_PN532_PIN_IRQ      4
#define BOARD_PN532_PIN_RST      2
#define BOARD_PN532_SPI_HZ      (5 * 1000 * 1000)   /* PN532 max ~5 MHz in SPI */

/*
 * PN5180 SPI — used when CONFIG_NFC_DRIVER_PN5180 is selected.
 *
 *   VCC      -> 5V (TX driver wants 5V; logic is 3.3V — most boards include
 *                   a level shifter, but verify with your specific module)
 *   GND      -> GND
 *   SCK      -> GPIO 18 (shared with PN532 mapping)
 *   MISO     -> GPIO 19
 *   MOSI     -> GPIO 23
 *   NSS      -> GPIO  5
 *   BUSY     -> GPIO 21  (NEW — must be polled before each command)
 *   RST      -> GPIO 22  (moved from GPIO 2 — needed for clean RF recovery)
 *   IRQ      -> GPIO  4  (status register drives this; we may use it as a
 *                         wake source for find_target later)
 *
 * Decoupling reminder: the PN5180 TX driver pulls ~100mA peaks. Put 10µF +
 * 100nF right at the chip's TVDD pin or you will see RF dropouts during
 * DESFire writes — a classic "card removed mid-personalize" ghost bug.
 */
#define BOARD_PN5180_SPI_HOST    VSPI_HOST
#define BOARD_PN5180_PIN_SCK     18
#define BOARD_PN5180_PIN_MISO    19
#define BOARD_PN5180_PIN_MOSI    23
#define BOARD_PN5180_PIN_NSS      5
#define BOARD_PN5180_PIN_BUSY    21
#define BOARD_PN5180_PIN_RST     22
#define BOARD_PN5180_PIN_IRQ      4
#define BOARD_PN5180_SPI_HZ     (7 * 1000 * 1000)   /* PN5180 max 7 MHz */

/* RS485 / OSDP */
#define BOARD_OSDP_UART_NUM      UART_NUM_2
#define BOARD_OSDP_PIN_TX       17
#define BOARD_OSDP_PIN_RX       16
#define BOARD_OSDP_BAUD         9600   /* match your panel; 9600 is typical */

/* Indicators */
#define BOARD_PIN_LED_GREEN     25
#define BOARD_PIN_LED_RED       26
#define BOARD_PIN_BUZZER        27
#define BOARD_PIN_BUTTON        32

/* OSDP identity */
#define BOARD_OSDP_PD_ADDRESS    0x65   /* default PD address; change per panel cfg */
