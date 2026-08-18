cmake_minimum_required(VERSION 3.16)

get_filename_component(NP2PRESENTATION_REPO_ROOT
                       "${CMAKE_CURRENT_LIST_DIR}/../../.." REALPATH)

idf_component_register(
    SRCS "${NP2PRESENTATION_REPO_ROOT}/host/presentation/np2_presentation.c"
    INCLUDE_DIRS "${NP2PRESENTATION_REPO_ROOT}/host/presentation")

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wall
    -Wextra
    -Werror=implicit-function-declaration)
