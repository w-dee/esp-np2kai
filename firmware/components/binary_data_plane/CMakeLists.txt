idf_component_register(
    SRCS
        "binary_codec.cpp"
        "cobs.cpp"
        "crc32.cpp"
        "transfer_manager.cpp"
    INCLUDE_DIRS "include"
)

target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu++20)
