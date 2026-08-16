idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS ""
    PRIV_REQUIRES uart_control_transport esp_app_format np2memoryprobe
                  np2fixtureprobe np2test_runner storage_fatfs storage_fatfs_probe)

target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu++20)

# The ordinary build stays formal. The reduced profile is enabled only by an
# explicit CMake cache option used by the dedicated emulator harness.
if(NP2_REDUCED_EXTMEM8)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE NP2_REDUCED_EXTMEM8=1)
endif()
if(STORAGE_FATFS_PROBE)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE STORAGE_FATFS_PROBE=1)
endif()
