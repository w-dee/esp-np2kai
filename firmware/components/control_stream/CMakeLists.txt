idf_component_register(
    SRCS "control_stream.cpp"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES control_plane binary_data_plane
)

target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu++20)
