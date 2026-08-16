cmake_minimum_required(VERSION 3.16)

get_filename_component(NP2LINKPROBE_REPO_ROOT
                       "${CMAKE_CURRENT_LIST_DIR}/../../.." REALPATH)

idf_component_register(
    SRCS "np2_link_probe.c"
    INCLUDE_DIRS
        "${NP2LINKPROBE_REPO_ROOT}/host/compat"
        "${NP2LINKPROBE_REPO_ROOT}/third_party/np2kai/src"
        "${NP2LINKPROBE_REPO_ROOT}/third_party/np2kai/src/i286c"
    PRIV_REQUIRES np2core np2host)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -std=c11
    -Wall
    -Wextra
    -Werror=implicit-function-declaration)

# The probe is link-only scaffolding.  This forces its object from the
# component archive into the final application without calling it at runtime.
target_link_options(${COMPONENT_LIB} INTERFACE
    "-Wl,--undefined=np2_phase2_link_probe")
