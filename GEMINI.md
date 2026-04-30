# OSDP LEAF Reader - Project History & Instructions

## Summary of Refactor (April 2026)
The reader was transformed from a non-buildable skeleton into a production-ready firmware.

### Major Accomplishments
1.  **Build System Stabilization**:
    *   Wrapped `libosdp` as a local component (`components/libosdp`) with a custom `CMakeLists.txt` and generated `osdp_config.h`.
    *   Renamed the conflicting local `console` component to `app_console` to avoid shadowing ESP-IDF's internal console logic.
    *   Resolved circular dependencies between the NFC HAL and the specific hardware drivers.
2.  **PN5180 Driver Completion**:
    *   Fully implemented ISO 14443-3 (anticollision loop) and ISO 14443-4 (APDU framing, chaining, and WTX).
    *   Corrected critical bitmask errors in PCB handling (R-block ACK/NAK confusion).
3.  **Security Hardening**:
    *   Implemented **Encrypted NVS** using the ESP32's hardware flash encryption.
    *   Site master keys, WiFi credentials, and OSDP states are now cryptographically protected.
    *   Increased partition table offset to `0x10000` to accommodate the larger secure bootloader.
4.  **Advanced Management**:
    *   **Serial Console**: Added commands for `osdp status`, `osdp setup`, `nfc status`, and a `self_test` logic check.
    *   **WebUI**: Integrated full OSDP configuration parity and real-time connectivity indicators.
    *   **mDNS**: Device is discoverable at `http://leaf-reader.local/`.
5.  **Reliability**:
    *   Implemented a physical **10-second factory reset** via the mode button to recover from misconfiguration.

### Architectural Conventions
*   **Shared Headers**: Internal task headers (`osdp_pd.h`, `nfc_task.h`) live in `components/reader_core/include` to ensure they are visible to all dependent components via the ESP-IDF component manager.
*   **Networking**: WiFi management logic (saving creds, status) is strictly contained in the `netconfig` component. UI and Console layers must use the `netconfig.h` API.
*   **Security**: Always prioritize `nvs_flash_secure_init()` for sensitive data.

### Known Configuration
*   **Default OSDP Address**: 0x65
*   **mDNS Hostname**: `leaf-reader`
*   **AP Provisioning SSID**: `leaf-reader-XXXXXX` (last 3 bytes of MAC)
