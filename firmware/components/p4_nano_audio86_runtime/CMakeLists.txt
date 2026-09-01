idf_component_register(
    SRCS "p4_nano_audio86_runtime.cpp"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES np2audio86_fixture esp_system freertos heap)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -std=gnu++20 -Wall -Wextra -Werror -ffunction-sections -fdata-sections)
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    P4_NANO_AUDIO86_RUNTIME_FOUNDATION_PROFILE=1)
