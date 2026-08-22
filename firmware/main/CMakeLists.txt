set(NP2_VIDEO_PROFILE_ACTIVE FALSE)
if(NP2_VIDEO_PROFILE OR "$ENV{NP2_VIDEO_PROFILE}" STREQUAL "1")
    set(NP2_VIDEO_PROFILE_ACTIVE TRUE)
endif()

set(NP2_PRESENTATION_PROFILE_ACTIVE FALSE)
if(NP2_PRESENTATION_PROFILE OR "$ENV{NP2_PRESENTATION_PROFILE}" STREQUAL "1")
    set(NP2_PRESENTATION_PROFILE_ACTIVE TRUE)
endif()

set(UART_SDMMC_PROFILE_ACTIVE FALSE)
if(UART_SDMMC_PROFILE OR "$ENV{UART_SDMMC_PROFILE}" STREQUAL "1")
    set(UART_SDMMC_PROFILE_ACTIVE TRUE)
endif()

set(NP2_SD_NP2TEST_PROFILE_ACTIVE FALSE)
if(NP2_SD_NP2TEST_PROFILE OR "$ENV{NP2_SD_NP2TEST_PROFILE}" STREQUAL "1")
    set(NP2_SD_NP2TEST_PROFILE_ACTIVE TRUE)
endif()

if(NP2_PRESENTATION_PROFILE_ACTIVE AND NP2_VIDEO_PROFILE_ACTIVE)
    message(FATAL_ERROR
            "NP2_PRESENTATION_PROFILE and NP2_VIDEO_PROFILE are mutually exclusive")
endif()

if(UART_SDMMC_PROFILE_ACTIVE AND
   (UART_FATFS_PROFILE OR NP2_VFS_FIXTURE_PROFILE))
    message(FATAL_ERROR
            "UART_SDMMC_PROFILE is mutually exclusive with UART_FATFS_PROFILE and NP2_VFS_FIXTURE_PROFILE")
endif()

if(NP2_SD_NP2TEST_PROFILE_ACTIVE AND
   (UART_SDMMC_PROFILE OR UART_SDMMC_PROFILE_ACTIVE OR UART_FATFS_PROFILE OR
    NP2_VFS_FIXTURE_PROFILE OR NP2_VIDEO_PROFILE OR NP2_VIDEO_PROFILE_ACTIVE OR
    NP2_PRESENTATION_PROFILE OR NP2_PRESENTATION_PROFILE_ACTIVE OR
    NP2_DOSIO_PROBE OR STORAGE_FATFS_PROBE OR STORAGE_FATFS_CI_BOUNDED OR
    NP2_REDUCED_EXTMEM8))
    message(FATAL_ERROR
            "NP2_SD_NP2TEST_PROFILE is mutually exclusive with other profiles, probes, and reduced EXTMEM")
endif()

if((NP2_VIDEO_PROFILE_ACTIVE OR NP2_PRESENTATION_PROFILE_ACTIVE) AND
   (NP2_VFS_FIXTURE_PROFILE OR UART_FATFS_PROFILE OR UART_SDMMC_PROFILE_ACTIVE OR
    NP2_SD_NP2TEST_PROFILE_ACTIVE OR
    NP2_DOSIO_PROBE OR
    STORAGE_FATFS_PROBE))
    message(FATAL_ERROR
            "video/presentation profiles are mutually exclusive with probe/storage profiles")
endif()

set(NP2_MAIN_PRIV_REQUIRES esp_app_format)
if(NP2_PRESENTATION_PROFILE_ACTIVE)
    list(APPEND NP2_MAIN_PRIV_REQUIRES np2presentation_probe)
elseif(NP2_VIDEO_PROFILE_ACTIVE)
    list(APPEND NP2_MAIN_PRIV_REQUIRES uart_control_transport file_transfer
         np2video_runner)
else()
    list(APPEND NP2_MAIN_PRIV_REQUIRES uart_control_transport file_transfer)
endif()
if(NP2_VIDEO_PROFILE_ACTIVE)
    # The video profile has no normal-probe dependencies.
elseif(NP2_PRESENTATION_PROFILE_ACTIVE)
    # The presentation profile has no fixture, storage, or NP2TEST dependencies.
else()
    list(APPEND NP2_MAIN_PRIV_REQUIRES np2memoryprobe np2test_runner)
    if(NP2_SD_NP2TEST_PROFILE_ACTIVE)
        list(APPEND NP2_MAIN_PRIV_REQUIRES storage_fatfs)
    else()
        list(APPEND NP2_MAIN_PRIV_REQUIRES np2fixtureprobe np2dosio_probe
             storage_fatfs storage_fatfs_probe)
    endif()
endif()
if(UART_SDMMC_PROFILE_ACTIVE OR NP2_SD_NP2TEST_PROFILE_ACTIVE)
    list(APPEND NP2_MAIN_PRIV_REQUIRES storage_sdmmc)
endif()

idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS ""
    PRIV_REQUIRES ${NP2_MAIN_PRIV_REQUIRES})

target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu++20)

# The ordinary build stays formal. The reduced profile is enabled only by an
# explicit CMake cache option used by the dedicated emulator harness.
if(NP2_REDUCED_EXTMEM8)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE NP2_REDUCED_EXTMEM8=1)
endif()
if(NP2_VIDEO_PROFILE_ACTIVE)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE NP2_VIDEO_PROFILE=1)
endif()
if(NP2_PRESENTATION_PROFILE_ACTIVE)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE NP2_PRESENTATION_PROFILE=1)
endif()
if(STORAGE_FATFS_PROBE)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE STORAGE_FATFS_PROBE=1)
endif()
if(NP2_DOSIO_PROBE)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE NP2_DOSIO_PROBE=1)
endif()
if(UART_FATFS_PROFILE)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE UART_FATFS_PROFILE=1)
endif()
if(UART_SDMMC_PROFILE_ACTIVE)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE UART_SDMMC_PROFILE=1)
endif()
if(NP2_SD_NP2TEST_PROFILE_ACTIVE)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE NP2_SD_NP2TEST_PROFILE=1)
endif()
if(NP2_VFS_FIXTURE_PROFILE)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE NP2_VFS_FIXTURE_PROFILE=1)
endif()
