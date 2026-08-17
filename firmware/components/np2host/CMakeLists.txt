cmake_minimum_required(VERSION 3.16)

# The Step 4 backend inventory remains the sole source of truth.  The stage
# annotations classify its eleven project-owned translation units into seven
# portable direct reuses and four ESP32-P4 replacements.
get_filename_component(NP2HOST_REPO_ROOT
                       "${CMAKE_CURRENT_LIST_DIR}/../../.." REALPATH)
set(NP2HOST_BACKEND_LIST
    "${NP2HOST_REPO_ROOT}/host/probe/host_backend_sources.txt")
set(NP2HOST_HOST_COMPAT "${NP2HOST_REPO_ROOT}/host/compat")
set(NP2HOST_VENDOR_SRC "${NP2HOST_REPO_ROOT}/third_party/np2kai/src")

if(NOT EXISTS "${NP2HOST_BACKEND_LIST}")
    message(FATAL_ERROR "np2host: authoritative backend inventory is missing")
endif()

file(STRINGS "${NP2HOST_BACKEND_LIST}" NP2HOST_BACKEND_LINES)
set(NP2HOST_CURRENT_STAGE "")
set(NP2HOST_ALL_SOURCES "")
set(NP2HOST_REUSABLE_SOURCES "")
set(NP2HOST_TIMING_SOURCE "")
set(NP2HOST_TIMEMNG_SOURCE "")
set(NP2HOST_TASKMNG_SOURCE "")
set(NP2HOST_DOSIO_SOURCE "")
set(NP2HOST_SEEN_STAGES "")

foreach(NP2HOST_BACKEND_LINE IN LISTS NP2HOST_BACKEND_LINES)
    string(STRIP "${NP2HOST_BACKEND_LINE}" NP2HOST_ENTRY)
    if(NP2HOST_ENTRY STREQUAL "")
        continue()
    endif()
    if(NP2HOST_ENTRY MATCHES "^# stage:[ \t]*(.+)$")
        set(NP2HOST_CURRENT_STAGE "${CMAKE_MATCH_1}")
        list(APPEND NP2HOST_SEEN_STAGES "${NP2HOST_CURRENT_STAGE}")
        continue()
    endif()
    if(NP2HOST_ENTRY MATCHES "^#")
        continue()
    endif()
    if(NP2HOST_CURRENT_STAGE STREQUAL "")
        message(FATAL_ERROR
                "np2host: source-list entry has no stage: ${NP2HOST_ENTRY}")
    endif()

    set(NP2HOST_SOURCE "${NP2HOST_REPO_ROOT}/${NP2HOST_ENTRY}")
    if(NOT EXISTS "${NP2HOST_SOURCE}" OR IS_DIRECTORY "${NP2HOST_SOURCE}")
        message(FATAL_ERROR "np2host: inventory source is missing: ${NP2HOST_SOURCE}")
    endif()
    list(APPEND NP2HOST_ALL_SOURCES "${NP2HOST_ENTRY}")

    if(NP2HOST_CURRENT_STAGE STREQUAL "headless-input" OR
       NP2HOST_CURRENT_STAGE STREQUAL "headless-audio" OR
       NP2HOST_CURRENT_STAGE STREQUAL "headless-system" OR
       NP2HOST_CURRENT_STAGE STREQUAL "headless-font" OR
       NP2HOST_CURRENT_STAGE STREQUAL "headless-screen" OR
       NP2HOST_CURRENT_STAGE STREQUAL "headless-communication")
        list(APPEND NP2HOST_REUSABLE_SOURCES "${NP2HOST_SOURCE}")
    elseif(NP2HOST_CURRENT_STAGE STREQUAL "headless-timing")
        if(NOT NP2HOST_TIMING_SOURCE STREQUAL "")
            message(FATAL_ERROR "np2host: duplicate timing backend entry")
        endif()
        set(NP2HOST_TIMING_SOURCE "${NP2HOST_SOURCE}")
    elseif(NP2HOST_CURRENT_STAGE STREQUAL "headless-calendar")
        if(NOT NP2HOST_TIMEMNG_SOURCE STREQUAL "")
            message(FATAL_ERROR "np2host: duplicate calendar backend entry")
        endif()
        set(NP2HOST_TIMEMNG_SOURCE "${NP2HOST_SOURCE}")
    elseif(NP2HOST_CURRENT_STAGE STREQUAL "headless-task")
        if(NOT NP2HOST_TASKMNG_SOURCE STREQUAL "")
            message(FATAL_ERROR "np2host: duplicate task backend entry")
        endif()
        set(NP2HOST_TASKMNG_SOURCE "${NP2HOST_SOURCE}")
    elseif(NP2HOST_CURRENT_STAGE STREQUAL "headless-dosio")
        if(NOT NP2HOST_DOSIO_SOURCE STREQUAL "")
            message(FATAL_ERROR "np2host: duplicate DOSIO backend entry")
        endif()
        set(NP2HOST_DOSIO_SOURCE "${NP2HOST_SOURCE}")
    else()
        message(FATAL_ERROR
                "np2host: unsupported backend inventory stage: ${NP2HOST_CURRENT_STAGE}")
    endif()
endforeach()

list(LENGTH NP2HOST_ALL_SOURCES NP2HOST_SOURCE_COUNT)
list(LENGTH NP2HOST_REUSABLE_SOURCES NP2HOST_REUSABLE_COUNT)
if(NOT NP2HOST_SOURCE_COUNT EQUAL 11 OR NOT NP2HOST_REUSABLE_COUNT EQUAL 7)
    message(FATAL_ERROR
            "np2host: expected 11 inventory sources and 7 reusable sources, got "
            "${NP2HOST_SOURCE_COUNT} and ${NP2HOST_REUSABLE_COUNT}")
endif()
foreach(NP2HOST_REQUIRED_STAGE IN ITEMS
        headless-input
        headless-audio
        headless-timing
        headless-calendar
        headless-system
        headless-task
        headless-dosio
        headless-font
        headless-screen
        headless-communication)
    list(FIND NP2HOST_SEEN_STAGES "${NP2HOST_REQUIRED_STAGE}" NP2HOST_STAGE_INDEX)
    if(NP2HOST_STAGE_INDEX LESS 0)
        message(FATAL_ERROR "np2host: required inventory stage is missing: ${NP2HOST_REQUIRED_STAGE}")
    endif()
endforeach()
if(NP2HOST_TIMING_SOURCE STREQUAL "" OR
   NP2HOST_TIMEMNG_SOURCE STREQUAL "" OR
   NP2HOST_TASKMNG_SOURCE STREQUAL "" OR
   NP2HOST_DOSIO_SOURCE STREQUAL "")
    message(FATAL_ERROR "np2host: incomplete ESP replacement classification")
endif()

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             "${NP2HOST_BACKEND_LIST}"
             ${NP2HOST_REUSABLE_SOURCES})

idf_component_register(
    SRCS
        ${NP2HOST_REUSABLE_SOURCES}
        "${CMAKE_CURRENT_LIST_DIR}/timing_esp.c"
        "${CMAKE_CURRENT_LIST_DIR}/timemng_esp.c"
        "${CMAKE_CURRENT_LIST_DIR}/taskmng_esp.c"
        "${CMAKE_CURRENT_LIST_DIR}/dosio_esp.c"
        "${CMAKE_CURRENT_LIST_DIR}/scrnmng_storage_esp.c"
    INCLUDE_DIRS
        "${CMAKE_CURRENT_LIST_DIR}/include"
        "${NP2HOST_HOST_COMPAT}"
        "${NP2HOST_VENDOR_SRC}"
        "${NP2HOST_VENDOR_SRC}/i286c"
    PRIV_REQUIRES esp_timer esp_psram esp_hw_support heap)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    DISABLE_SOUND
    SUPPORT_KAI_IMAGES
    SUPPORT_FAST_MEMORYCHECK
    SUPPORT_16BPP
    BYTESEX_LITTLE
    OSLANG_UTF8
    SUPPORT_UTF8
    NP2_FIRMWARE_DOSIO
    "NP2KAI_GIT_TAG=\"0.86.0.22\""
    "NP2KAI_GIT_HASH=\"e2dc9046aa5c786fcfbfb87e883457e421026e31\"")

target_compile_options(${COMPONENT_LIB} PRIVATE
    -std=c11
    -Wall
    -Wextra
    -fno-common
    -Wno-error=parentheses
    -Wno-error=maybe-uninitialized
    -Wno-error=incompatible-pointer-types
    -Wno-error=unused-value
    -Wno-error=unknown-pragmas
    -Wno-error=format
    -Wno-error=address
    -Wno-error=return-type
    -Werror=implicit-function-declaration)

target_compile_options(${COMPONENT_LIB} PRIVATE
    "SHELL:-iquote ${NP2HOST_VENDOR_SRC}"
    "SHELL:-iquote ${NP2HOST_VENDOR_SRC}/bios"
    "SHELL:-iquote ${NP2HOST_VENDOR_SRC}/cbus"
    "SHELL:-iquote ${NP2HOST_VENDOR_SRC}/io")
