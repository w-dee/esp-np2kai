idf_component_register(
    SRCS
        "binary_codec.cpp"
        "cobs.cpp"
        "crc32.cpp"
        "test_endpoint.cpp"
        "transfer_manager.cpp"
    INCLUDE_DIRS "include"
)

target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu++20)

if(NP2_EMU_TEST)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE NP2_EMU_TEST=1)
endif()
