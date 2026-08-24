cmake_minimum_required(VERSION 3.16)

get_filename_component(P4_NANO_USB_HOST_REPO_ROOT
                       "${CMAKE_CURRENT_LIST_DIR}/../../../../.." REALPATH)
list(APPEND EXTRA_COMPONENT_DIRS
     "${P4_NANO_USB_HOST_REPO_ROOT}/firmware/components/hid_boot_keyboard")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(p4_nano_usb_host)
