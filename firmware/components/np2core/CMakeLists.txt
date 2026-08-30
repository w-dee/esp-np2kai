cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED NP2_I286C_INLINE_MEM_FASTPATH)
    set(NP2_I286C_INLINE_MEM_FASTPATH "0")
endif()
if(NOT NP2_I286C_INLINE_MEM_FASTPATH STREQUAL "0" AND
   NOT NP2_I286C_INLINE_MEM_FASTPATH STREQUAL "1")
    message(FATAL_ERROR
            "np2core: NP2_I286C_INLINE_MEM_FASTPATH must be exactly 0 or 1")
endif()

# 86R.1 owns a deliberately narrow, opt-in PC-9801-86 guest/audio source
# boundary.  The current production target remains sound-disabled; the
# feature flag is exported as a compile contract for later source slices.
if(NOT DEFINED NP2_ASYNC_AUDIO86)
    set(NP2_ASYNC_AUDIO86 "0")
endif()
if(NOT NP2_ASYNC_AUDIO86 STREQUAL "0" AND
   NOT NP2_ASYNC_AUDIO86 STREQUAL "1")
    message(FATAL_ERROR
            "np2core: NP2_ASYNC_AUDIO86 must be exactly 0 or 1")
endif()

set(NP2_PCCORE_PHASE_PROFILER_ACTIVE FALSE)
if(NP2VIDEO_BENCHMARK_PROFILE OR
   "$ENV{NP2VIDEO_BENCHMARK_PROFILE}" STREQUAL "1")
    set(NP2_PCCORE_PHASE_PROFILER_ACTIVE TRUE)
endif()

# Phase 1 consumes the Step 4 inventory and preparation contract directly.
# The generated patched sources stay below the component build directory.
get_filename_component(NP2CORE_REPO_ROOT
                       "${CMAKE_CURRENT_LIST_DIR}/../../.." REALPATH)
set(NP2CORE_VENDOR_ROOT "${NP2CORE_REPO_ROOT}/third_party/np2kai")
set(NP2CORE_VENDOR_SRC "${NP2CORE_VENDOR_ROOT}/src")
set(NP2CORE_HOST_COMPAT "${NP2CORE_REPO_ROOT}/host/compat")
set(NP2CORE_SOURCE_LIST "${NP2CORE_REPO_ROOT}/host/probe/host_core_sources.txt")
set(NP2CORE_PREPARER "${NP2CORE_REPO_ROOT}/host/tools/prepare_step4_sources.py")
set(NP2CORE_PATCH_SET "${NP2CORE_REPO_ROOT}/host/patches/np2kai/step4/patch-set.json")
set(NP2CORE_IMPORT_MANIFEST "${NP2CORE_VENDOR_ROOT}/import-manifest.json")
set(NP2CORE_VENDOR_VERIFY "${NP2CORE_REPO_ROOT}/tools/np2kai/verify_np2kai.py")
set(NP2CORE_PREPARED_ROOT "${CMAKE_CURRENT_BINARY_DIR}/step4-prepared")

if(NP2_ASYNC_AUDIO86 STREQUAL "1")
    # Feature-on builds consume the separately pinned PC-9801-86 guest source
    # closure.  Keep it prepared below the component build directory just as
    # Step 4, and never compile pristine vendor paths for this boundary.
    set(NP2CORE_AUDIO_SOURCE_LIST
        "${NP2CORE_REPO_ROOT}/host/probe/audio86_guest_sources.txt")
    set(NP2CORE_AUDIO_PATCH_SET
        "${NP2CORE_REPO_ROOT}/host/patches/np2kai/audio86/patch-set.json")
    set(NP2CORE_AUDIO_PREPARED_ROOT
        "${CMAKE_CURRENT_BINARY_DIR}/audio86-prepared")
    foreach(NP2CORE_AUDIO_REQUIRED_INPUT IN ITEMS
            NP2CORE_AUDIO_SOURCE_LIST NP2CORE_AUDIO_PATCH_SET)
        if(NOT EXISTS "${${NP2CORE_AUDIO_REQUIRED_INPUT}}")
            message(FATAL_ERROR
                    "np2core: required audio86 input is missing: "
                    "${${NP2CORE_AUDIO_REQUIRED_INPUT}}")
        endif()
    endforeach()
endif()

foreach(NP2CORE_REQUIRED_INPUT IN ITEMS
        NP2CORE_SOURCE_LIST
        NP2CORE_PREPARER
        NP2CORE_PATCH_SET
        NP2CORE_IMPORT_MANIFEST
        NP2CORE_VENDOR_VERIFY)
    if(NOT EXISTS "${${NP2CORE_REQUIRED_INPUT}}")
        message(FATAL_ERROR
                "np2core: required Step 4 input is missing: ${${NP2CORE_REQUIRED_INPUT}}")
    endif()
endforeach()

find_package(Python3 COMPONENTS Interpreter REQUIRED)

# Parse the authoritative source list; do not duplicate its 124 entries here.
file(STRINGS "${NP2CORE_SOURCE_LIST}" NP2CORE_SOURCE_LINES)
set(NP2CORE_LOGICAL_SOURCES)
set(NP2CORE_VENDOR_INPUTS)
foreach(NP2CORE_SOURCE_LINE IN LISTS NP2CORE_SOURCE_LINES)
    string(STRIP "${NP2CORE_SOURCE_LINE}" NP2CORE_LOGICAL_SOURCE)
    if(NP2CORE_LOGICAL_SOURCE STREQUAL "" OR
       NP2CORE_LOGICAL_SOURCE MATCHES "^#")
        continue()
    endif()
    string(REGEX REPLACE "#.*$" "" NP2CORE_LOGICAL_SOURCE
                         "${NP2CORE_LOGICAL_SOURCE}")
    string(STRIP "${NP2CORE_LOGICAL_SOURCE}" NP2CORE_LOGICAL_SOURCE)
    if(NP2CORE_LOGICAL_SOURCE STREQUAL "" OR
       NP2CORE_LOGICAL_SOURCE MATCHES "[ \t]")
        message(FATAL_ERROR
                "np2core: invalid source-list entry: ${NP2CORE_SOURCE_LINE}")
    endif()
    list(FIND NP2CORE_LOGICAL_SOURCES "${NP2CORE_LOGICAL_SOURCE}"
         NP2CORE_DUPLICATE_INDEX)
    if(NP2CORE_DUPLICATE_INDEX GREATER -1)
        message(FATAL_ERROR
                "np2core: duplicate source-list entry: ${NP2CORE_LOGICAL_SOURCE}")
    endif()
    set(NP2CORE_PRISTINE_SOURCE
        "${NP2CORE_VENDOR_SRC}/${NP2CORE_LOGICAL_SOURCE}")
    if(NOT EXISTS "${NP2CORE_PRISTINE_SOURCE}" OR
       IS_DIRECTORY "${NP2CORE_PRISTINE_SOURCE}")
        message(FATAL_ERROR
                "np2core: source-list file is missing: ${NP2CORE_PRISTINE_SOURCE}")
    endif()
    list(APPEND NP2CORE_LOGICAL_SOURCES "${NP2CORE_LOGICAL_SOURCE}")
    list(APPEND NP2CORE_VENDOR_INPUTS "${NP2CORE_PRISTINE_SOURCE}")
endforeach()

list(LENGTH NP2CORE_LOGICAL_SOURCES NP2CORE_SOURCE_COUNT)
if(NOT NP2CORE_SOURCE_COUNT EQUAL 124)
    message(FATAL_ERROR
            "np2core: expected exactly 124 Step 4 sources, got ${NP2CORE_SOURCE_COUNT}")
endif()

# Derive patched-source paths from the immutable Step 4 patch set. This is
# only a mapping; the preparer remains responsible for validation and patching.
file(STRINGS "${NP2CORE_PATCH_SET}" NP2CORE_PATCH_SOURCE_LINES
     REGEX "\\\"logical_source\\\"")
file(STRINGS "${NP2CORE_PATCH_SET}" NP2CORE_PATCH_PATH_LINES
     REGEX "\\\"patch_path\\\"")
set(NP2CORE_PATCHED_SOURCES)
set(NP2CORE_PATCHED_HEADERS)
set(NP2CORE_PATCH_FILES)
get_filename_component(NP2CORE_PATCH_ROOT "${NP2CORE_PATCH_SET}" DIRECTORY)
foreach(NP2CORE_PATCH_SOURCE_LINE IN LISTS NP2CORE_PATCH_SOURCE_LINES)
    string(REGEX MATCH
           "\\\"logical_source\\\"[ \t]*:[ \t]*\\\"([^\\\"]+)\\\""
           NP2CORE_PATCH_MATCH "${NP2CORE_PATCH_SOURCE_LINE}")
    if(NOT CMAKE_MATCH_1)
        message(FATAL_ERROR
                "np2core: cannot parse logical_source from patch set")
    endif()
    set(NP2CORE_PATCH_LOGICAL_SOURCE "${CMAKE_MATCH_1}")
    list(FIND NP2CORE_PATCHED_SOURCES "${NP2CORE_PATCH_LOGICAL_SOURCE}"
         NP2CORE_PATCH_DUPLICATE_INDEX)
    if(NP2CORE_PATCH_DUPLICATE_INDEX GREATER -1)
        message(FATAL_ERROR
                "np2core: duplicate patch-set source: ${NP2CORE_PATCH_LOGICAL_SOURCE}")
    endif()
    list(FIND NP2CORE_LOGICAL_SOURCES "${NP2CORE_PATCH_LOGICAL_SOURCE}"
         NP2CORE_PATCH_SOURCE_INDEX)
    if(NP2CORE_PATCH_SOURCE_INDEX LESS 0)
        if(NOT NP2CORE_PATCH_LOGICAL_SOURCE MATCHES "\\.h$")
            message(FATAL_ERROR
                    "np2core: patch-set source is outside the authoritative inventory: "
                    "${NP2CORE_PATCH_LOGICAL_SOURCE}")
        endif()
        list(APPEND NP2CORE_PATCHED_SOURCES "${NP2CORE_PATCH_LOGICAL_SOURCE}")
        list(APPEND NP2CORE_PATCHED_HEADERS "${NP2CORE_PATCH_LOGICAL_SOURCE}")
        list(APPEND NP2CORE_VENDOR_INPUTS
             "${NP2CORE_VENDOR_SRC}/${NP2CORE_PATCH_LOGICAL_SOURCE}")
        continue()
    endif()
    list(APPEND NP2CORE_PATCHED_SOURCES "${NP2CORE_PATCH_LOGICAL_SOURCE}")
endforeach()
foreach(NP2CORE_PATCH_PATH_LINE IN LISTS NP2CORE_PATCH_PATH_LINES)
    string(REGEX MATCH
           "\\\"patch_path\\\"[ \t]*:[ \t]*\\\"([^\\\"]+)\\\""
           NP2CORE_PATCH_MATCH "${NP2CORE_PATCH_PATH_LINE}")
    if(NOT CMAKE_MATCH_1)
        message(FATAL_ERROR
                "np2core: cannot parse patch_path from patch set")
    endif()
    set(NP2CORE_PATCH_FILE "${NP2CORE_PATCH_ROOT}/${CMAKE_MATCH_1}")
    get_filename_component(NP2CORE_PATCH_FILE "${NP2CORE_PATCH_FILE}" REALPATH)
    if(NOT EXISTS "${NP2CORE_PATCH_FILE}" OR
       IS_DIRECTORY "${NP2CORE_PATCH_FILE}")
        message(FATAL_ERROR
                "np2core: patch file is missing: ${NP2CORE_PATCH_FILE}")
    endif()
    list(APPEND NP2CORE_PATCH_FILES "${NP2CORE_PATCH_FILE}")
endforeach()
list(LENGTH NP2CORE_PATCHED_SOURCES NP2CORE_PATCH_COUNT)
list(LENGTH NP2CORE_PATCH_FILES NP2CORE_PATCH_FILE_COUNT)
if(NOT NP2CORE_PATCH_COUNT EQUAL NP2CORE_PATCH_FILE_COUNT)
    message(FATAL_ERROR
            "np2core: patch-set source/path entry count mismatch")
endif()

# Any relevant input change forces CMake to rerun the deterministic preparer
# before the source paths below are consumed. This prevents stale generated
# patched sources from being silently compiled.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             "${NP2CORE_SOURCE_LIST}"
             "${NP2CORE_PREPARER}"
             "${NP2CORE_PATCH_SET}"
             "${NP2CORE_IMPORT_MANIFEST}"
             "${NP2CORE_VENDOR_VERIFY}"
             ${NP2CORE_VENDOR_INPUTS}
             ${NP2CORE_PATCH_FILES})

if(NP2_ASYNC_AUDIO86 STREQUAL "1")
    file(STRINGS "${NP2CORE_AUDIO_SOURCE_LIST}" NP2CORE_AUDIO_SOURCE_LINES)
    set(NP2CORE_AUDIO_LOGICAL_SOURCES)
    foreach(NP2CORE_AUDIO_SOURCE_LINE IN LISTS NP2CORE_AUDIO_SOURCE_LINES)
        string(STRIP "${NP2CORE_AUDIO_SOURCE_LINE}" NP2CORE_AUDIO_LOGICAL_SOURCE)
        if(NP2CORE_AUDIO_LOGICAL_SOURCE STREQUAL "" OR
           NP2CORE_AUDIO_LOGICAL_SOURCE MATCHES "^#")
            continue()
        endif()
        string(REGEX REPLACE "#.*$" "" NP2CORE_AUDIO_LOGICAL_SOURCE
                             "${NP2CORE_AUDIO_LOGICAL_SOURCE}")
        string(STRIP "${NP2CORE_AUDIO_LOGICAL_SOURCE}" NP2CORE_AUDIO_LOGICAL_SOURCE)
        if(NP2CORE_AUDIO_LOGICAL_SOURCE STREQUAL "" OR
           NP2CORE_AUDIO_LOGICAL_SOURCE MATCHES "[ \t]")
            message(FATAL_ERROR
                    "np2core: invalid audio86 source-list entry: "
                    "${NP2CORE_AUDIO_SOURCE_LINE}")
        endif()
        list(FIND NP2CORE_AUDIO_LOGICAL_SOURCES
             "${NP2CORE_AUDIO_LOGICAL_SOURCE}" NP2CORE_AUDIO_DUPLICATE_INDEX)
        if(NP2CORE_AUDIO_DUPLICATE_INDEX GREATER -1)
            message(FATAL_ERROR
                    "np2core: duplicate audio86 source-list entry: "
                    "${NP2CORE_AUDIO_LOGICAL_SOURCE}")
        endif()
        if(NOT EXISTS "${NP2CORE_VENDOR_SRC}/${NP2CORE_AUDIO_LOGICAL_SOURCE}")
            message(FATAL_ERROR
                    "np2core: audio86 source-list file is missing: "
                    "${NP2CORE_VENDOR_SRC}/${NP2CORE_AUDIO_LOGICAL_SOURCE}")
        endif()
        list(APPEND NP2CORE_AUDIO_LOGICAL_SOURCES
             "${NP2CORE_AUDIO_LOGICAL_SOURCE}")
    endforeach()
    list(LENGTH NP2CORE_AUDIO_LOGICAL_SOURCES NP2CORE_AUDIO_SOURCE_COUNT)
    if(NOT NP2CORE_AUDIO_SOURCE_COUNT EQUAL 2)
        message(FATAL_ERROR
                "np2core: expected exactly 2 audio86 sources, got "
                "${NP2CORE_AUDIO_SOURCE_COUNT}")
    endif()
    set(NP2CORE_AUDIO_VENDOR_INPUTS)
    foreach(NP2CORE_AUDIO_LOGICAL_SOURCE IN LISTS NP2CORE_AUDIO_LOGICAL_SOURCES)
        list(APPEND NP2CORE_AUDIO_VENDOR_INPUTS
             "${NP2CORE_VENDOR_SRC}/${NP2CORE_AUDIO_LOGICAL_SOURCE}")
    endforeach()
    set(NP2CORE_AUDIO_PATCH_FILES
        "${NP2CORE_REPO_ROOT}/host/patches/np2kai/audio86/0001-board86-guest-adapter.patch"
        "${NP2CORE_REPO_ROOT}/host/patches/np2kai/audio86/0002-pcm86io-guest-adapter.patch")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                 "${NP2CORE_AUDIO_SOURCE_LIST}"
                 "${NP2CORE_AUDIO_PATCH_SET}"
                 ${NP2CORE_AUDIO_VENDOR_INPUTS}
                 ${NP2CORE_AUDIO_PATCH_FILES})
    execute_process(
        COMMAND "${Python3_EXECUTABLE}" "${NP2CORE_PREPARER}"
                --vendor-root "${NP2CORE_VENDOR_ROOT}"
                --import-manifest "${NP2CORE_IMPORT_MANIFEST}"
                --patch-set "${NP2CORE_AUDIO_PATCH_SET}"
                --output-root "${NP2CORE_AUDIO_PREPARED_ROOT}"
        RESULT_VARIABLE NP2CORE_AUDIO_PREPARE_RESULT
        OUTPUT_VARIABLE NP2CORE_AUDIO_PREPARE_OUTPUT
        ERROR_VARIABLE NP2CORE_AUDIO_PREPARE_ERROR)
    if(NOT NP2CORE_AUDIO_PREPARE_RESULT EQUAL 0)
        message(FATAL_ERROR
                "np2core: audio86 source preparation failed (result "
                "${NP2CORE_AUDIO_PREPARE_RESULT})\n"
                "${NP2CORE_AUDIO_PREPARE_OUTPUT}\n"
                "${NP2CORE_AUDIO_PREPARE_ERROR}")
    endif()
endif()

execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${NP2CORE_VENDOR_VERIFY}"
            --vendor-root "${NP2CORE_VENDOR_ROOT}"
    RESULT_VARIABLE NP2CORE_VERIFY_RESULT
    OUTPUT_VARIABLE NP2CORE_VERIFY_OUTPUT
    ERROR_VARIABLE NP2CORE_VERIFY_ERROR)
if(NOT NP2CORE_VERIFY_RESULT EQUAL 0)
    message(FATAL_ERROR
            "np2core: Step 4 vendor verification failed (result ${NP2CORE_VERIFY_RESULT})\n"
            "${NP2CORE_VERIFY_OUTPUT}\n${NP2CORE_VERIFY_ERROR}")
endif()

execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${NP2CORE_PREPARER}"
            --vendor-root "${NP2CORE_VENDOR_ROOT}"
            --import-manifest "${NP2CORE_IMPORT_MANIFEST}"
            --patch-set "${NP2CORE_PATCH_SET}"
            --output-root "${NP2CORE_PREPARED_ROOT}"
    RESULT_VARIABLE NP2CORE_PREPARE_RESULT
    OUTPUT_VARIABLE NP2CORE_PREPARE_OUTPUT
    ERROR_VARIABLE NP2CORE_PREPARE_ERROR)
if(NOT NP2CORE_PREPARE_RESULT EQUAL 0)
    message(FATAL_ERROR
            "np2core: Step 4 source preparation failed (result ${NP2CORE_PREPARE_RESULT})\n"
            "${NP2CORE_PREPARE_OUTPUT}\n${NP2CORE_PREPARE_ERROR}")
endif()

set(NP2CORE_PREPARED_COMPILE_ROOT
    "${CMAKE_CURRENT_BINARY_DIR}/step4-compile-src")
set(NP2CORE_I286C_HEADER_PATCH_INDEX)
list(FIND NP2CORE_PATCHED_HEADERS "i286c/i286c.h"
     NP2CORE_I286C_HEADER_PATCH_INDEX)
if(NP2CORE_I286C_HEADER_PATCH_INDEX GREATER -1)
    # A quoted include resolves relative to the source file before -iquote
    # directories.  Compile generated copies of the I286 translation units
    # beside the patched header so every shared i286c.h consumer sees the
    # project-owned overlay without editing third_party in place.
    file(MAKE_DIRECTORY "${NP2CORE_PREPARED_COMPILE_ROOT}/i286c")
    configure_file(
        "${NP2CORE_PREPARED_ROOT}/patched-src/i286c/i286c.h"
        "${NP2CORE_PREPARED_COMPILE_ROOT}/i286c/i286c.h"
        COPYONLY)
endif()

set(NP2CORE_SOURCES)
foreach(NP2CORE_LOGICAL_SOURCE IN LISTS NP2CORE_LOGICAL_SOURCES)
    list(FIND NP2CORE_PATCHED_SOURCES "${NP2CORE_LOGICAL_SOURCE}"
         NP2CORE_PATCH_INDEX)
    if(NP2CORE_PATCH_INDEX GREATER -1)
        set(NP2CORE_SOURCE
            "${NP2CORE_PREPARED_ROOT}/patched-src/${NP2CORE_LOGICAL_SOURCE}")
        if(NOT EXISTS "${NP2CORE_SOURCE}" OR IS_DIRECTORY "${NP2CORE_SOURCE}")
            message(FATAL_ERROR
                    "np2core: prepared source is missing: ${NP2CORE_SOURCE}")
        endif()
    elseif(NP2CORE_I286C_HEADER_PATCH_INDEX GREATER -1 AND
           NP2CORE_LOGICAL_SOURCE MATCHES "^i286c/.*\\.c$")
        set(NP2CORE_SOURCE
            "${NP2CORE_PREPARED_COMPILE_ROOT}/${NP2CORE_LOGICAL_SOURCE}")
        configure_file(
            "${NP2CORE_VENDOR_SRC}/${NP2CORE_LOGICAL_SOURCE}"
            "${NP2CORE_SOURCE}"
            COPYONLY)
    else()
        set(NP2CORE_SOURCE "${NP2CORE_VENDOR_SRC}/${NP2CORE_LOGICAL_SOURCE}")
    endif()
    list(APPEND NP2CORE_SOURCES "${NP2CORE_SOURCE}")
endforeach()

if(NP2_ASYNC_AUDIO86 STREQUAL "1")
    foreach(NP2CORE_AUDIO_LOGICAL_SOURCE IN LISTS NP2CORE_AUDIO_LOGICAL_SOURCES)
        set(NP2CORE_AUDIO_SOURCE
            "${NP2CORE_AUDIO_PREPARED_ROOT}/patched-src/${NP2CORE_AUDIO_LOGICAL_SOURCE}")
        if(NOT EXISTS "${NP2CORE_AUDIO_SOURCE}" OR
           IS_DIRECTORY "${NP2CORE_AUDIO_SOURCE}")
            message(FATAL_ERROR
                    "np2core: prepared audio86 source is missing: "
                    "${NP2CORE_AUDIO_SOURCE}")
        endif()
        list(APPEND NP2CORE_SOURCES "${NP2CORE_AUDIO_SOURCE}")
    endforeach()
endif()

idf_component_register(
    SRCS ${NP2CORE_SOURCES} "np2_pccore_profiler.c"
    LDFRAGMENTS linker.lf
    REQUIRES esp_timer np2audio86_guest_adapter
    INCLUDE_DIRS
        "include"
        "${NP2CORE_HOST_COMPAT}"
        "${NP2CORE_VENDOR_SRC}"
        "${NP2CORE_VENDOR_SRC}/i286c")

# Keep the Step 4 compiler contract for this ESP32-P4 compile-only checkpoint.
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    DISABLE_SOUND
    SUPPORT_KAI_IMAGES
    SUPPORT_FAST_MEMORYCHECK
    SUPPORT_16BPP
    BYTESEX_LITTLE
    OSLANG_UTF8
    SUPPORT_UTF8
    NP2_FIRMWARE_DOSIO
    "NP2_ASYNC_AUDIO86=${NP2_ASYNC_AUDIO86}"
    "NP2_I286C_INLINE_MEM_FASTPATH=${NP2_I286C_INLINE_MEM_FASTPATH}"
    "NP2KAI_GIT_TAG=\"0.86.0.22\""
    "NP2KAI_GIT_HASH=\"e2dc9046aa5c786fcfbfb87e883457e421026e31\"")

if(NP2_PCCORE_PHASE_PROFILER_ACTIVE)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        NP2_PCCORE_PHASE_PROFILER=1)
endif()

target_compile_options(${COMPONENT_LIB} PRIVATE
    -std=c11
    -Wall
    -Wextra
    -fno-common
    # ESP-IDF adds -Werror=all globally; Step 4 only promotes implicit
    # function declarations, so neutralize the broader IDF defaults here.
    -Wno-error=parentheses
    -Wno-error=maybe-uninitialized
    -Wno-error=incompatible-pointer-types
    -Wno-error=unused-value
    -Wno-error=unknown-pragmas
    -Wno-error=format
    -Wno-error=address
    -Wno-error=return-type
    -Werror=implicit-function-declaration)

# The Step 4 probe supplies the original source directory with -iquote for
# each prepared source. Reproduce that include resolution for the patched
# source directories without copying headers into the generated tree.
target_compile_options(${COMPONENT_LIB} PRIVATE
    "SHELL:-iquote ${NP2CORE_VENDOR_SRC}"
    "SHELL:-iquote ${NP2CORE_VENDOR_SRC}/bios"
    "SHELL:-iquote ${NP2CORE_VENDOR_SRC}/cbus"
    "SHELL:-iquote ${NP2CORE_VENDOR_SRC}/io"
    "SHELL:-iquote ${NP2CORE_VENDOR_SRC}/vram")
