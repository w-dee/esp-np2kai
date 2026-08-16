idf_component_register(SRCS "main.cpp"
                       INCLUDE_DIRS ""
                       PRIV_REQUIRES uart_control_transport esp_app_format np2linkprobe np2memoryprobe np2fixtureprobe np2host)

target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu++20)

# Phase 2 link-probe scaffolding: main retains the probe component in the final
# link, but does not call any NP2 lifecycle entry point at runtime.
