idf_component_register(SRCS "main.cpp"
                       INCLUDE_DIRS ""
                       PRIV_REQUIRES uart_control_transport esp_app_format np2linkprobe np2memoryprobe np2fixtureprobe np2fixture np2test_runner np2host)

target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu++20)

# The ordinary build stays formal. The reduced profile is enabled only by an
# explicit CMake cache option used by the dedicated emulator harness.
if(NP2_REDUCED_EXTMEM8)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE NP2_REDUCED_EXTMEM8=1)
endif()

# The Phase 2 link probe remains retained as independent bring-up evidence.
