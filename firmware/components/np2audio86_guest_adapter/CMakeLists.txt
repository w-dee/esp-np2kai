cmake_minimum_required(VERSION 3.16)

# 86R.2 provides the bounded, synchronous Domain-G guest model.  It has no
# dependency on the audio generator or on a worker/task implementation.
idf_component_register(
    SRCS "np2audio86_guest_adapter.c"
    INCLUDE_DIRS "include")
