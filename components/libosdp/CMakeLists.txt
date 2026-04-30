set(LIB_OSDP_SOURCES
    "src/osdp_common.c"
    "src/osdp_phy.c"
    "src/osdp_pd.c"
    "src/osdp_sc.c"
    "src/osdp_file.c"
    "src/osdp_cp.c"
    "src/osdp_diag.c"
    "src/crypto/mbedtls.c"
)

set(LIB_OSDP_UTILS_SRC
    "utils/src/utils.c"
    "utils/src/list.c"
    "utils/src/queue.c"
    "utils/src/slab.c"
    "utils/src/disjoint_set.c"
    "utils/src/logger.c"
    "utils/src/crc16.c"
)

idf_component_register(
    SRCS ${LIB_OSDP_SOURCES} ${LIB_OSDP_UTILS_SRC}
    INCLUDE_DIRS "include" "src" "utils/include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES mbedtls
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE "-DOPT_OSDP_USE_MBEDTLS")
target_compile_definitions(${COMPONENT_LIB} PRIVATE "-D__linux__") # Trick c-utils into using posix/time.h
