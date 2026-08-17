idf_component_register(
    SRCS "storage_fatfs_probe.cpp"
    INCLUDE_DIRS "include"
    REQUIRES storage_fatfs mbedtls
)

target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu++20)

if(STORAGE_FATFS_PROBE)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE STORAGE_FATFS_TEST_HOOKS=1)
endif()

if(STORAGE_FATFS_CI_BOUNDED)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE STORAGE_FATFS_CI_BOUNDED=1)
endif()
