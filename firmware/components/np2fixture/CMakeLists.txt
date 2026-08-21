cmake_minimum_required(VERSION 3.16)

get_filename_component(NP2FIXTURE_REPO_ROOT
                       "${CMAKE_CURRENT_LIST_DIR}/../../.." REALPATH)
set(NP2FIXTURE_VENDOR_SRC "${NP2FIXTURE_REPO_ROOT}/third_party/np2kai/src")
set(NP2FIXTURE_HOST_COMPAT "${NP2FIXTURE_REPO_ROOT}/host/compat")

idf_component_register(
    SRCS "np2_fixture.c" "np2_fixture_vfs.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_partition
    PRIV_REQUIRES mbedtls np2host)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    SUPPORT_KAI_IMAGES
    SUPPORT_FAST_MEMORYCHECK
    BYTESEX_LITTLE
    OSLANG_UTF8
    SUPPORT_UTF8
    NP2_FIRMWARE_DOSIO)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -std=c11
    -Wall
    -Wextra
    -Werror=implicit-function-declaration
    "SHELL:-iquote ${NP2FIXTURE_HOST_COMPAT}"
    "SHELL:-iquote ${NP2FIXTURE_VENDOR_SRC}"
    "SHELL:-iquote ${NP2FIXTURE_VENDOR_SRC}/i286c")
