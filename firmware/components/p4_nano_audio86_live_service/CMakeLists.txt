idf_component_register(
    SRCS
        "p4_nano_audio86_live_service.c"
        "p4_nano_audio86_live_service_profile.c"
    INCLUDE_DIRS "include"
    REQUIRES
        np2audio86_core
        np2audio86_guest_adapter
        np2opngen_fixture
        np2pcm_output)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -std=gnu11
    -Wall
    -Wextra
    -Werror)

if(P4_NANO_AUDIO86_LIVE_SERVICE_PROFILE)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        P4_NANO_AUDIO86_LIVE_SERVICE_PROFILE=1)
endif()
