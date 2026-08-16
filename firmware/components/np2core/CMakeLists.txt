cmake_minimum_required(VERSION 3.16)

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
    list(FIND NP2CORE_LOGICAL_SOURCES "${NP2CORE_PATCH_LOGICAL_SOURCE}"
         NP2CORE_PATCH_SOURCE_INDEX)
    if(NP2CORE_PATCH_SOURCE_INDEX LESS 0)
        message(FATAL_ERROR
                "np2core: patch-set source is outside the authoritative inventory: "
                "${NP2CORE_PATCH_LOGICAL_SOURCE}")
    endif()
    list(FIND NP2CORE_PATCHED_SOURCES "${NP2CORE_PATCH_LOGICAL_SOURCE}"
         NP2CORE_PATCH_DUPLICATE_INDEX)
    if(NP2CORE_PATCH_DUPLICATE_INDEX GREATER -1)
        message(FATAL_ERROR
                "np2core: duplicate patch-set source: ${NP2CORE_PATCH_LOGICAL_SOURCE}")
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
    else()
        set(NP2CORE_SOURCE "${NP2CORE_VENDOR_SRC}/${NP2CORE_LOGICAL_SOURCE}")
    endif()
    list(APPEND NP2CORE_SOURCES "${NP2CORE_SOURCE}")
endforeach()

idf_component_register(
    SRCS ${NP2CORE_SOURCES}
    INCLUDE_DIRS
        "${NP2CORE_HOST_COMPAT}"
        "${NP2CORE_VENDOR_SRC}"
        "${NP2CORE_VENDOR_SRC}/i286c")

# Keep the Step 4 compiler contract for this ESP32-P4 compile-only checkpoint.
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    DISABLE_SOUND
    SUPPORT_KAI_IMAGES
    SUPPORT_FAST_MEMORYCHECK
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
    "SHELL:-iquote ${NP2CORE_VENDOR_SRC}/io")
