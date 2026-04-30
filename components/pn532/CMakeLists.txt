idf_component_register(
    SRCS "pn532.c"
    INCLUDE_DIRS "include"
    REQUIRES driver freertos esp_timer log board nfc_hal
)
