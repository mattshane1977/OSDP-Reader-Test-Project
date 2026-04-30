idf_component_register(
    SRCS "leaf.c"
    INCLUDE_DIRS "include"
    REQUIRES log nvs_flash mbedtls desfire
)
