# Copyright 2020-2026 Yonik Seeley and Luxir contributors
# SPDX-License-Identifier: Apache-2.0

find_package(Protobuf CONFIG REQUIRED)
find_package(gRPC CONFIG REQUIRED)
find_package(Threads)

# The protobuf/gRPC C++ codegen target (myproto) is gone with the concrete luxir::api
# cutover: no TU includes luxir*.pb.h or luxir*.grpc.pb.h anymore, so it was compiling four
# large generated .cc files into a static library nothing linked. The trait-templated hpp
# luxir::proto generation (luxir_proto_hpp) went the same way. The wire model is
# luxir_proto_concrete (below); descriptor-only reflection is luxir_fds_gen (below).
# _pb_inc (protobuf's include dir) is still needed by the FileDescriptorSet protoc command.
get_target_property(_pb_inc protobuf::libprotobuf INTERFACE_INCLUDE_DIRECTORIES)

# --- FileDescriptorSet for descriptor-only reflection ---
# Embedded into luxir_fds_data.h, consumed by src/luxir/server/luxir_descriptors.cpp
# (registerLuxirDescriptors), which replaces myproto's static descriptor registration
# now that the generated message classes are gone. Lands in ${CMAKE_CURRENT_BINARY_DIR}
# (=${CMAKE_BINARY_DIR}/protos); luxir_lib puts that on its include path (root CMakeLists).
set(_fds "${CMAKE_CURRENT_BINARY_DIR}/luxir_fds.bin")
add_custom_command(
  OUTPUT "${_fds}"
  COMMAND protobuf::protoc --descriptor_set_out=${_fds} --include_imports
          -I "${CMAKE_CURRENT_SOURCE_DIR}" -I "${_pb_inc}"
          "${CMAKE_CURRENT_SOURCE_DIR}/luxir_types.proto" "${CMAKE_CURRENT_SOURCE_DIR}/luxir.proto"
  DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/luxir_types.proto" "${CMAKE_CURRENT_SOURCE_DIR}/luxir.proto"
  COMMENT "descriptor-only: generating FileDescriptorSet"
  VERBATIM)
set(_fds_hdr "${CMAKE_CURRENT_BINARY_DIR}/luxir_fds_data.h")
add_custom_command(
  OUTPUT "${_fds_hdr}"
  COMMAND ${CMAKE_COMMAND} -DFDS_IN=${_fds} -DOUT_H=${_fds_hdr} -P "${CMAKE_SOURCE_DIR}/cmake/embed_fds.cmake"
  DEPENDS "${_fds}" "${CMAKE_SOURCE_DIR}/cmake/embed_fds.cmake"
  COMMENT "descriptor-only: embedding FileDescriptorSet"
  VERBATIM)
add_custom_target(luxir_fds_gen DEPENDS "${_fds_hdr}")

# --- concrete luxir::api classes (Phase 2 cutover wire model) ---
# Out-of-line metadata (.pb.cpp/.json.cpp) binding pb_meta + glz::meta to the hand-written
# concrete headers in src/luxir/api. This is the engine's wire model post-cutover; it
# coexists with the templated luxir_proto_hpp (distinct namespace luxir::api) through the
# migration. luxir_lib links it once engine TUs include the concrete headers; the concrete
# round-trip tests (test_concrete_proto/) link it now.
find_package(glaze CONFIG REQUIRED)   # concrete .json.cpp pulls <hpp_proto/json.hpp> -> glaze
set(_concrete_gen "${CMAKE_CURRENT_BINARY_DIR}/concrete")
hpp_proto_generate(OUT_VAR _concrete_srcs OUT_DIR "${_concrete_gen}" CONCRETE SNAKE_JSON
  CONCRETE_NAMESPACE luxir.api   # -> namespace luxir::api (coexists with templated luxir::proto)
  IMPORT_DIRS "${CMAKE_CURRENT_SOURCE_DIR}" "${_pb_inc}"
  PROTOS "${CMAKE_CURRENT_SOURCE_DIR}/luxir_types.proto"
         "${CMAKE_CURRENT_SOURCE_DIR}/luxir_index.proto")
# EXCLUDE_FROM_ALL until the engine flip: built on demand (the concrete tests link it);
# at flip time luxir_lib links it and it joins the default build.
add_library(luxir_proto_concrete STATIC EXCLUDE_FROM_ALL ${_concrete_srcs})
# src/luxir/api: the hand-written concrete headers the metadata #includes, plus build.h.
target_include_directories(luxir_proto_concrete PUBLIC "${CMAKE_SOURCE_DIR}/src/luxir/api")
target_link_libraries(luxir_proto_concrete PUBLIC hpp_proto::runtime glaze::glaze)
set_target_properties(luxir_proto_concrete PROPERTIES CXX_STANDARD 26 CXX_STANDARD_REQUIRED ON)

# The generated .json.cpp emits `#include "luxir_types.hpp"` (the wire MODEL, glaze-free) and
# then instantiates glaze over those types. The from/to overrides and the Luxir JSON dialect
# live in luxir_types_json.hpp, which the generator has no way to name - force-include it so
# the dialect is in scope before any glz::meta below. The .pb.cpp (binary codec) is
# deliberately left alone: it must not pay for glaze.
foreach(_src ${_concrete_srcs})
  if(_src MATCHES "luxir_types\\.json\\.cpp$")
    set_source_files_properties("${_src}" TARGET_DIRECTORY luxir_proto_concrete PROPERTIES
      COMPILE_OPTIONS "-include;${CMAKE_SOURCE_DIR}/src/luxir/api/luxir_types_json.hpp")
  endif()
endforeach()
