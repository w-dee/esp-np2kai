idf_component_register(
    SRCS "file_transfer.cpp" "path.cpp" "zero_rle_v1.cpp"
    INCLUDE_DIRS "include"
    REQUIRES storage binary_data_plane
    PRIV_REQUIRES mbedtls
)

target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu++20)
