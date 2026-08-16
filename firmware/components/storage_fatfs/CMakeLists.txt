idf_component_register(
    SRCS "storage_fatfs.cpp"
    INCLUDE_DIRS "include"
    REQUIRES storage fatfs wear_levelling vfs
)

target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu++20)
