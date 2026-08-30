cmake_minimum_required(VERSION 3.16)

# 86R.1 is an API-only ownership seam.  Runtime implementation is deferred
# to the later 86R slices; this component exports only the opaque contract.
idf_component_register(INCLUDE_DIRS "include")
