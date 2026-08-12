# The following lines of boilerplate have to be in this project's
# CMakeLists in this exact order for CMake to work correctly.
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

idf_build_set_property(MINIMAL_BUILD ON)

project(esp_np2kai)

