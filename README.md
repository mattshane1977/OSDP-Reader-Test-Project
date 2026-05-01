# osdp-leaf-reader

ESP-IDF firmware for an OSDP v2.2 PD that reads and writes LEAF-style
credentials on MIFARE DESFire EV3 cards.

## Hardware

See [SCHEMATIC.md](SCHEMATIC.md) for the full system diagram and pinout.

| Function     | Component                | Notes |
|--------------|--------------------------|-------|
| MCU          | ESP32-S3-WROOM-1 N16R8   | Dual core; OSDP on core 0, NFC on core 1 |
| RFID         | PN5180 (SPI) or PN532    | Selected at build time via Kconfig |
| RS485        | HiLetgo TTL→485 auto     | 3.3V logic safe; A/B to OSDP bus |
| Indicators   | On-board WS2812B RGB LED | GPIO 48 — no external hardware needed |
| Buzzer       | Active piezo             | GPIO 21 on/off |
| Mode button  | BOOT button to GND       | GPIO 0 with internal pull-up |

Pin maps for both NFC chips live in `components/board/include/board.h`.

## NFC driver selection

```
idf.py menuconfig
  → OSDP LEAF reader → NFC front-end driver
```

or set in `sdkconfig.defaults`:

```
CONFIG_NFC_DRIVER_PN5180=y    # default — ISO 14443-4, DESFire EV3
CONFIG_NFC_DRIVER_PN532=y     # alternative
```

The desfire and leaf layers call `nfc_*` functions exclusively; swapping the
chip is a pure component-level change with no edits to higher layers.

## Driver status

**PN532**: complete and working. Used for original bring-up.

**PN5180**: complete and refined. Implements ISO 14443-3 anticollision loop (CL1/CL2/CL3) and ISO 14443-4 I-block exchange with chaining and WTX. Tested for single-card presence.

## Build

```
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

## What works on first boot

* OSDP PD comes up at address 0x65, baud 9600, with a dev SCBK. Talks to
  any OSDP CP (panel, OSDP-CLI, etc.).
* Idle: slow green heartbeat.
* Short-press button: enter Read mode (solid green). Present a personalized
  card — green flash + beep on grant, red double-beep on deny. Card data is
  submitted to the panel as an `OSDP_EVENT_CARDREAD`.
* Long-press button (≥2s): enter Write mode (slow red blink). Present a
  factory-default DESFire EV3 card — alternating green/red triple-beep on
  successful personalization.

## Site key

A development site key is auto-provisioned on first boot. Replace it before
deployment — easiest path is a serial console command or the UI.

## OSDP credential format

The PD submits a 10-byte raw payload to the CP per card read:

```
facility(2 LE) | card_id(4 LE) | issue_date(4 LE)
```

LEAF readers in production submit a vendor-specific framing; replace
`post_card_read()` in `osdp_pd.c` if you want to mimic a specific format.

## Factory Reset

If you lose access to the device (incorrect WiFi or OSDP settings), press and
hold the physical mode button for **10 seconds**. The reader will indicate an
error (red blink + long beep), wipe all NVS partitions (including site keys),
and reboot to factory defaults.

## Security

This reader implements **Encrypted NVS**. The site master key, WiFi
credentials, and OSDP persistent state are protected by the ESP32 hardware
flash encryption. On first boot, the device generates a unique NVS encryption
key and stores it in a dedicated encrypted partition (`nvs_keys`).

## Dev surfaces

The reader exposes two control surfaces during development. Both reach the
same mode controller and event bus. mDNS is active; you can reach the device
at `http://leaf-reader.local/` on most networks.

### Serial console (UART0)

Connect via USB, open serial at 115200. Type `help` for the command list.
Key commands:
- `status` — full system health (uptime, NFC, OSDP, WiFi)
- `osdp status` — check CP connection and configured address/baud
- `osdp setup <addr> <baud>` — persistently configure OSDP parameters
- `nfc status` — verify RFID chip initialization and driver name
- `key set <32-hex>` — update the site master key in encrypted NVS

```
> status
osdp-leaf-reader v1.0.0
mode    : idle
uptime  : 42 s
nfc     : pn5180 (ready)
osdp    : online (connected to CP)
wifi    : MySSID rssi=-45 ip=192.168.1.50
OK status
```

Lines starting with `=` are JSON event push notifications, suitable for
piping into a host-side tool. Output prefixes:
- `> ` — echo of your command
- `OK ` / `ERR ` — command result
- `=` followed by JSON — async event
- everything else — normal `esp_log` output

### Web UI

On first boot the device has no WiFi credentials, so it brings up an open
SoftAP named `leaf-reader-XXXXXX`. Connect to it from your laptop, browse
to `http://192.168.4.1/` (or `http://leaf-reader.local/`), and use the WiFi
setup page (or the serial `wifi setup <ssid> <pass>` command) to provision STA
credentials. Reboot, and the device joins your LAN.

The UI shows current mode, lets you switch modes, queue enrollments, set
the site key, and shows the live event stream.

## Layout

```
osdp-leaf-reader/
├── CMakeLists.txt
├── partitions.csv
├── sdkconfig.defaults
├── main/
│   ├── app_main.c
│   ├── nfc_task.c
│   ├── osdp_pd.c
│   ├── Kconfig.projbuild        # NFC driver selection menu
│   └── CMakeLists.txt
└── components/
    ├── board/         (LEDs, buzzer, button, pin map for both chips)
    ├── reader_core/   (mode_controller — multi-subscriber event bus)
    ├── nfc_hal/       (driver-agnostic NFC interface)
    ├── pn532/         (PN532 SPI driver — complete)
    ├── pn5180/        (PN5180 SPI driver — complete with ISO 14443-4)
    ├── desfire/       (EV2-First auth, CMAC, encrypted-mode wrapper)
    ├── leaf/          (credential layout + AN10922 diversification)
    ├── netconfig/     (WiFi STA with SoftAP fallback + mDNS)
    ├── app_console/   (UART0 line-based dev console)
    └── webui/         (HTTP + WebSocket dev UI)
```

### From either machine

The Debian build VM and Windows dev machine can both reach the device on
the LAN — same URL works from either. Serial is Windows-only since that's
where the USB cable lives. Both surfaces work concurrently — multiple
event subscribers fan out from the mode controller.
