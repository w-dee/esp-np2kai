cmake_minimum_required(VERSION 3.16)

# 86R.2 provides the bounded, synchronous Domain-G guest model.  It has no
# dependency on the audio generator or on a worker/task implementation.
idf_component_register(
    SRCS "np2audio86_guest_adapter.c"
    INCLUDE_DIRS "include")

# Keep the adapter compile command on the same explicit feature boundary as
# the prepared guest sources.  The feature remains compile/link-only here;
# normal production startup does not activate the PC-9801-86 runtime.
if(NOT DEFINED NP2_ASYNC_AUDIO86)
    set(NP2_ASYNC_AUDIO86 "0")
endif()
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    DISABLE_SOUND
    "NP2_ASYNC_AUDIO86=${NP2_ASYNC_AUDIO86}")
