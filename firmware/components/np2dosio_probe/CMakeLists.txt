cmake_minimum_required(VERSION 3.16)

idf_component_register(
    SRCS "np2dosio_probe.cpp"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES mbedtls np2fixture np2host storage_fatfs
)

target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu++20)
