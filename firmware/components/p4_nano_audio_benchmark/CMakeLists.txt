get_filename_component(NP2_AUDIO_REPO_ROOT
    "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
set(NP2_AUDIO_FIXTURE
    "${NP2_AUDIO_REPO_ROOT}/testdata/s98/retrofm-pocket-demo-strict.s98")
set(NP2_AUDIO_EMBED_FIXTURE ${CMAKE_CURRENT_BINARY_DIR}/retrofm-pocket-demo-strict.s98)
configure_file("${NP2_AUDIO_FIXTURE}" "${NP2_AUDIO_EMBED_FIXTURE}" COPYONLY)

idf_component_register(
    SRCS "p4_nano_audio_benchmark.cpp"
         "${NP2_AUDIO_REPO_ROOT}/host/compat/np2_sha256.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES np2opngen_fixture esp_timer esp_system freertos
    EMBED_FILES "${NP2_AUDIO_EMBED_FIXTURE}")

set(P4_NANO_AUDIO_OPT "debug" CACHE STRING
    "Optimization mode for the P4-NANO audio benchmark pipeline")
set_property(CACHE P4_NANO_AUDIO_OPT PROPERTY STRINGS debug o2)
if(NOT P4_NANO_AUDIO_ONLY_BENCHMARK_PROFILE)
    if(P4_NANO_AUDIO_OPT STREQUAL "o2")
        message(FATAL_ERROR
            "P4_NANO_AUDIO_OPT=o2 requires P4_NANO_AUDIO_ONLY_BENCHMARK_PROFILE")
    elseif(NOT P4_NANO_AUDIO_OPT STREQUAL "debug")
        message(FATAL_ERROR
            "P4_NANO_AUDIO_OPT must be debug or o2, got: ${P4_NANO_AUDIO_OPT}")
    endif()
endif()

target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu++20)
if(P4_NANO_AUDIO_ONLY_BENCHMARK_PROFILE AND
   P4_NANO_AUDIO_OPT STREQUAL "o2")
    target_compile_options(${COMPONENT_LIB} PRIVATE -O2)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE P4_NANO_AUDIO_OPT_O2=1)
endif()

# The emulator correctness run intentionally omits the 60-second workload:
# esp-emu timing is not representative and the synthetic profile is too slow
# there.  This definition belongs to the benchmark component itself (a
# definition on the main component does not propagate to this target).
if(NP2_EMU_TEST)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE P4_AUDIO_EMU_TEST=1)
endif()
