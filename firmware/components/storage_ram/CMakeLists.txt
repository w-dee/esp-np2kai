idf_component_register(
    SRCS "storage_ram.cpp"
    INCLUDE_DIRS "include"
    REQUIRES storage
)

target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu++20)
