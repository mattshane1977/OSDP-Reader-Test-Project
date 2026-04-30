# Generate osdp_config.h from the upstream template.
# Upstream uses configure_file() in their own cmake build; we replicate that
# here with static values since ESP-IDF won't run the libosdp configure step.
set(PROJECT_VERSION "2.4.0")
set(PROJECT_NAME    "libosdp")
set(GIT_BRANCH      "")
set(GIT_REV         "")
set(GIT_TAG         "")
set(GIT_DIFF        "")
set(REPO_ROOT       "${CMAKE_CURRENT_LIST_DIR}")

# CMAKE_CURRENT_LIST_DIR (not CMAKE_CURRENT_SOURCE_DIR) is required here:
# ESP-IDF's requirements-gathering pass uses include() rather than
# add_subdirectory(), which causes CMAKE_CURRENT_SOURCE_DIR to point at the
# project build directory instead of the component source directory.
configure_file(
    "${CMAKE_CURRENT_LIST_DIR}/src/osdp_config.h.in"
    "${CMAKE_CURRENT_LIST_DIR}/src/osdp_config.h"
    @ONLY
)

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
    INCLUDE_DIRS "include" "utils/include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES mbedtls
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE "-DOPT_OSDP_USE_MBEDTLS")
target_compile_definitions(${COMPONENT_LIB} PRIVATE "-D__linux__") # Trick c-utils into using posix/time.h