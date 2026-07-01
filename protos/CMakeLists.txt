find_package(Protobuf CONFIG REQUIRED)
find_package(gRPC CONFIG REQUIRED)
find_package(Threads)

set(PROTO_FILES
    solux.proto
    solux_types.proto
)

add_library(myproto ${PROTO_FILES})
target_link_libraries(myproto
    PUBLIC
        protobuf::libprotobuf
        gRPC::grpc
        gRPC::grpc++
        gRPC::grpc++_reflection
)
target_include_directories(myproto PUBLIC ${CMAKE_CURRENT_BINARY_DIR})

get_target_property(grpc_cpp_plugin_location gRPC::grpc_cpp_plugin LOCATION)
protobuf_generate(TARGET myproto LANGUAGE cpp PROTOC_OPTIONS --experimental_editions)
protobuf_generate(TARGET myproto LANGUAGE grpc
        GENERATE_EXTENSIONS .grpc.pb.h .grpc.pb.cc
        PLUGIN "protoc-gen-grpc=${grpc_cpp_plugin_location}"
        PROTOC_OPTIONS --experimental_editions)

# The trait-templated hpp solux::proto generation (solux_proto_hpp / solux_proto_hpp_gen)
# was dropped after the concrete solux::api cutover: no TU includes "protos/solux*.pb.hpp"
# anymore. The concrete wire model is solux_proto_concrete (below); descriptor-only
# reflection is solux_fds_gen (below). _pb_inc (protobuf's include dir) is still needed by
# the FileDescriptorSet protoc command.
get_target_property(_pb_inc protobuf::libprotobuf INTERFACE_INCLUDE_DIRECTORIES)

# --- FileDescriptorSet for descriptor-only reflection ---
# Embedded into solux_fds_data.h, consumed by src/solux/server/solux_descriptors.cpp
# (registerSoluxDescriptors), which replaces myproto's static descriptor registration
# now that the generated message classes are gone. Lands in ${CMAKE_CURRENT_BINARY_DIR}
# (=${CMAKE_BINARY_DIR}/protos); solux_lib puts that on its include path (root CMakeLists).
set(_fds "${CMAKE_CURRENT_BINARY_DIR}/solux_fds.bin")
add_custom_command(
  OUTPUT "${_fds}"
  COMMAND protobuf::protoc --descriptor_set_out=${_fds} --include_imports
          -I "${CMAKE_CURRENT_SOURCE_DIR}" -I "${_pb_inc}"
          "${CMAKE_CURRENT_SOURCE_DIR}/solux_types.proto" "${CMAKE_CURRENT_SOURCE_DIR}/solux.proto"
  DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/solux_types.proto" "${CMAKE_CURRENT_SOURCE_DIR}/solux.proto"
  COMMENT "descriptor-only: generating FileDescriptorSet"
  VERBATIM)
set(_fds_hdr "${CMAKE_CURRENT_BINARY_DIR}/solux_fds_data.h")
add_custom_command(
  OUTPUT "${_fds_hdr}"
  COMMAND ${CMAKE_COMMAND} -DFDS_IN=${_fds} -DOUT_H=${_fds_hdr} -P "${CMAKE_SOURCE_DIR}/cmake/embed_fds.cmake"
  DEPENDS "${_fds}" "${CMAKE_SOURCE_DIR}/cmake/embed_fds.cmake"
  COMMENT "descriptor-only: embedding FileDescriptorSet"
  VERBATIM)
add_custom_target(solux_fds_gen DEPENDS "${_fds_hdr}")

# --- concrete solux::api classes (Phase 2 cutover wire model) ---
# Out-of-line metadata (.pb.cpp/.json.cpp) binding pb_meta + glz::meta to the hand-written
# concrete headers in src/solux/api. This is the engine's wire model post-cutover; it
# coexists with the templated solux_proto_hpp (distinct namespace solux::api) through the
# migration. solux_lib links it once engine TUs include the concrete headers; the concrete
# round-trip tests (test_concrete_proto/) link it now.
find_package(glaze CONFIG REQUIRED)   # concrete .json.cpp pulls <hpp_proto/json.hpp> -> glaze
set(_concrete_gen "${CMAKE_CURRENT_BINARY_DIR}/concrete")
hpp_proto_generate(OUT_VAR _concrete_srcs OUT_DIR "${_concrete_gen}" CONCRETE SNAKE_JSON
  CONCRETE_NAMESPACE solux.api   # -> namespace solux::api (coexists with templated solux::proto)
  IMPORT_DIRS "${CMAKE_CURRENT_SOURCE_DIR}" "${_pb_inc}"
  PROTOS "${CMAKE_CURRENT_SOURCE_DIR}/solux_types.proto"
         "${CMAKE_CURRENT_SOURCE_DIR}/solux.proto")
# EXCLUDE_FROM_ALL until the engine flip: built on demand (the concrete tests link it);
# at flip time solux_lib links it and it joins the default build.
add_library(solux_proto_concrete STATIC EXCLUDE_FROM_ALL ${_concrete_srcs})
# src/solux/api: the hand-written concrete headers the metadata #includes, plus build.h.
target_include_directories(solux_proto_concrete PUBLIC "${CMAKE_SOURCE_DIR}/src/solux/api")
target_link_libraries(solux_proto_concrete PUBLIC hpp_proto::runtime glaze::glaze)
set_target_properties(solux_proto_concrete PROPERTIES CXX_STANDARD 26 CXX_STANDARD_REQUIRED ON)
