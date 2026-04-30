idf_component_register(
    SRCS "console.c"
    INCLUDE_DIRS "include"
    REQUIRES freertos log esp_system esp_netif nvs_flash leaf reader_core esp_wifi netconfig
    PRIV_REQUIRES driver
)
