idf_component_register(SRCS "main.cpp"
                       INCLUDE_DIRS ""
                       PRIV_REQUIRES uart_control_transport esp_app_format np2core)

target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu++20)

# Phase 1 compile-check scaffolding: the direct dependency forces MINIMAL_BUILD
# to compile the authoritative np2core archive. No NP2 runtime call is made;
# later phases will replace this with main -> np2test_runner -> np2core.
