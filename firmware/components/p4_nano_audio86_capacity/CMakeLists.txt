idf_component_register(
    SRCS "p4_nano_audio86_capacity.cpp"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES np2audio86_fixture esp_timer esp_system freertos heap)

target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu++20 -O2 -Wno-error=format)
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    P4_NANO_AUDIO86_CAPACITY_PROFILE=1)

if(P4_AUDIO86_PROFILE_MODE OR "$ENV{P4_AUDIO86_PROFILE_MODE}" STREQUAL "1")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        P4_AUDIO86_PROFILE_MODE=1 NP2_AUDIO86_PROFILE=1)
endif()
if(NP2_EMU_TEST)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE P4_AUDIO_EMU_TEST=1)
endif()
if(P4_AUDIO86_GIT_SHA)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        P4_AUDIO86_GIT_SHA="${P4_AUDIO86_GIT_SHA}")
endif()
