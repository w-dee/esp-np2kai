idf_component_register(
    SRCS "dma2d_copy.cpp"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_hw_support esp_mm heap freertos)

target_compile_options(${COMPONENT_LIB} PRIVATE
    "$<$<COMPILE_LANGUAGE:CXX>:-std=gnu++20>"
    "$<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>"
    "$<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>"
    -Wall -Wextra -Werror)
