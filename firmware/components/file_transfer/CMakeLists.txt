idf_component_register(
    SRCS "file_transfer.cpp" "path.cpp"
    INCLUDE_DIRS "include"
    REQUIRES storage binary_data_plane
)

target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu++20)
