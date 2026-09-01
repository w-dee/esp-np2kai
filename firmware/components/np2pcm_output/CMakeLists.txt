idf_component_register(
    SRCS "np2pcm_output.c"
    INCLUDE_DIRS "include"
    REQUIRES np2opngen_fixture)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -std=gnu11
    -Wall
    -Wextra
    -Werror)
