idf_component_register(
    SRCS "webui.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server log freertos esp_wifi esp_netif nvs_flash leaf reader_core
    EMBED_FILES "assets/index.html"
)
