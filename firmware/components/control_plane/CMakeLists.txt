idf_component_register(
    SRCS
        "control_plane.cpp"
        "control_framing.cpp"
        "control_protocol.cpp"
        "command_dispatcher.cpp"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES json binary_data_plane
)

target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu++20)
