# Copyright 2020-2026 Yonik Seeley and Luxir contributors
# SPDX-License-Identifier: Apache-2.0

# hpp-proto build integration (vendored via deps/hpp-proto submodule + deps/is_utf8).
#
# Provides:
#   target  is_utf8                 - vendored UTF-8 validation (deps/is_utf8)
#   target  protoc-gen-hpp          - the code generator plugin, built from the
#                                     submodule (self-hosting: no libprotobuf needed)
#   target  hpp_proto::runtime      - INTERFACE: hpp-proto runtime headers + is_utf8
#   function hpp_proto_generate(...) - run the plugin to generate C++ headers
#
# The plugin and runtime build at the project C++ standard (C++26); they rely on the
# serializer C++26 fix carried on the deps/hpp-proto submodule's integration branch.

set(HPP_PROTO_DIR "${CMAKE_SOURCE_DIR}/deps/hpp-proto")
set(IS_UTF8_DIR "${CMAKE_SOURCE_DIR}/deps/is_utf8")

if(NOT EXISTS "${HPP_PROTO_DIR}/include/hpp_proto/binpb.hpp")
  message(FATAL_ERROR "deps/hpp-proto submodule not initialized (run: git submodule update --init)")
endif()

find_package(Protobuf CONFIG REQUIRED)  # for protobuf::protoc (the compiler front end only)

# --- vendored is_utf8 ---
add_library(is_utf8 STATIC "${IS_UTF8_DIR}/src/is_utf8.cpp")
target_include_directories(is_utf8 PUBLIC "${IS_UTF8_DIR}/include")
target_compile_features(is_utf8 PUBLIC cxx_std_17)
# is_utf8 selects its SIMD kernel at RUNTIME (cpuid -> AVX2/SSE4.2/AVX512 via
# __attribute__((target(...)))), so it needs no -march and stays portable. But the
# kernels must be optimized to be fast: at -O0 (the debug presets) even the dispatched
# AVX2 path runs ~80x slower (~0.85 vs ~65 GB/s, measured). Force release-grade
# optimization in every preset so validation is fast in debug/asan builds too. Do NOT
# add -march here: that would pin the binary to the build host and undercut dispatch.
target_compile_options(is_utf8 PRIVATE -O3)
# GCC 16 flags is_utf8's own push_options/pop_options target-region macros as a
# mismatched-pragma warning; it is the vendored dep's code, not ours, so keep it a
# warning rather than letting the tree's -Werror fail the (clean) asan build on it.
target_compile_options(is_utf8 PRIVATE -Wno-error=pragmas)

# --- runtime (header-only) interface for consumers of generated code ---
add_library(hpp_proto_runtime INTERFACE)
target_include_directories(hpp_proto_runtime SYSTEM INTERFACE "${HPP_PROTO_DIR}/include")
target_link_libraries(hpp_proto_runtime INTERFACE is_utf8)
target_compile_features(hpp_proto_runtime INTERFACE cxx_std_23)
add_library(hpp_proto::runtime ALIAS hpp_proto_runtime)

# --- the code generator plugin (self-hosting; links only is_utf8) ---
add_executable(protoc-gen-hpp "${HPP_PROTO_DIR}/src/protoc-plugin/hpp_gen.cpp")
target_include_directories(protoc-gen-hpp PRIVATE "${HPP_PROTO_DIR}/include")
target_link_libraries(protoc-gen-hpp PRIVATE is_utf8)
target_compile_features(protoc-gen-hpp PRIVATE cxx_std_23)
# This single TU is the head of the clean-build critical path: CMake gives every luxir_lib
# and luxir_test object an order-only dependency on it (they link luxir_proto_concrete,
# whose sources it generates), so nothing else starts until it links. It is a build-time
# tool we never debug and it runs in ~40ms, so drop debug info: -g0 takes it from ~14s to
# ~11.5s. Same reasoning as is_utf8's -O3 above - the vendored build tool gets the flags
# that suit it, not the tree's preset flags.
target_compile_options(protoc-gen-hpp PRIVATE -g0)

# hpp_proto_generate(
#   OUT_VAR <var>            # set in parent scope to the list of generated headers
#   OUT_DIR <dir>            # output root for generated headers (added to an include path by the caller)
#   PROTOS <p1> [p2 ...]     # .proto files (each must live under one of IMPORT_DIRS)
#   IMPORT_DIRS <d1> [d2 ...]# protoc -I search dirs (the dir containing a proto determines its output subpath)
#   [NAMESPACE_PREFIX <p>]   # hpp-proto namespace_prefix option (e.g. hpptest)
#   [SNAKE_JSON]             # the luxir JSON convention: keys are the proto
#                            # (snake_case) field names, one spelling only (no
#                            # camelCase aliases); an explicit [json_name = "..."]
#                            # override is still the primary key; enum values
#                            # serialize as lowercase names ("text", not "TEXT")
# )
function(hpp_proto_generate)
  cmake_parse_arguments(ARG "CONCRETE;SNAKE_JSON" "OUT_VAR;OUT_DIR;NAMESPACE_PREFIX;CONCRETE_NAMESPACE" "PROTOS;IMPORT_DIRS" ${ARGN})
  file(MAKE_DIRECTORY "${ARG_OUT_DIR}")

  set(_inc_flags)
  foreach(d ${ARG_IMPORT_DIRS})
    get_filename_component(_dabs "${d}" ABSOLUTE)
    list(APPEND _inc_flags -I "${_dabs}")
  endforeach()

  # Assemble the comma-separated plugin options. CONCRETE selects the hand-friendly
  # concrete-class metadata emission (.pb.cpp/.json.cpp bound to hand-written headers)
  # instead of the default trait-templated headers.
  set(_opts "")
  if(ARG_CONCRETE)
    set(_opts "concrete=true")
    if(ARG_CONCRETE_NAMESPACE)
      # dotted (e.g. luxir.api); the plugin converts dots to :: and retargets the
      # emitted metadata's namespace to it (coexists with the templated package ns).
      set(_opts "${_opts},concrete_namespace=${ARG_CONCRETE_NAMESPACE}")
    endif()
  endif()
  if(ARG_SNAKE_JSON)
    if(_opts)
      set(_opts "${_opts},preserve_proto_field_names=true,json_aliases=false,lowercase_enum_json=true")
    else()
      set(_opts "preserve_proto_field_names=true,json_aliases=false,lowercase_enum_json=true")
    endif()
  endif()
  if(ARG_NAMESPACE_PREFIX)
    if(_opts)
      set(_opts "${_opts},namespace_prefix=${ARG_NAMESPACE_PREFIX}")
    else()
      set(_opts "namespace_prefix=${ARG_NAMESPACE_PREFIX}")
    endif()
  endif()
  if(_opts)
    set(_hpp_out "${_opts}:${ARG_OUT_DIR}")
  else()
    set(_hpp_out "${ARG_OUT_DIR}")
  endif()

  set(_generated)
  foreach(proto ${ARG_PROTOS})
    get_filename_component(_abs "${proto}" ABSOLUTE)
    # find the import dir that contains this proto -> its output subpath mirrors that
    set(_rel "")
    foreach(d ${ARG_IMPORT_DIRS})
      get_filename_component(_dabs "${d}" ABSOLUTE)
      string(FIND "${_abs}" "${_dabs}/" _pos)
      if(_pos EQUAL 0)
        file(RELATIVE_PATH _rel "${_dabs}" "${_abs}")
        break()
      endif()
    endforeach()
    if(_rel STREQUAL "")
      message(FATAL_ERROR "hpp_proto_generate: ${_abs} is not under any IMPORT_DIRS")
    endif()
    string(REGEX REPLACE "\\.proto$" "" _stem "${_rel}")

    set(_outs)
    if(ARG_CONCRETE)
      # concrete mode emits out-of-line metadata translation units bound to the
      # hand-written headers: <stem>.pb.cpp (binary codec) + <stem>.json.cpp (glaze).
      list(APPEND _outs "${ARG_OUT_DIR}/${_stem}.pb.cpp" "${ARG_OUT_DIR}/${_stem}.json.cpp")
    else()
      foreach(ext pb msg glz desc)
        list(APPEND _outs "${ARG_OUT_DIR}/${_stem}.${ext}.hpp")
      endforeach()
    endif()

    add_custom_command(
      OUTPUT ${_outs}
      COMMAND protobuf::protoc
              --plugin=protoc-gen-hpp=$<TARGET_FILE:protoc-gen-hpp>
              --hpp_out=${_hpp_out} ${_inc_flags} "${_abs}"
      DEPENDS "${_abs}" protoc-gen-hpp
      COMMENT "hpp-proto: generating ${_stem} (concrete=${ARG_CONCRETE})"
      VERBATIM)
    list(APPEND _generated ${_outs})
  endforeach()

  set(${ARG_OUT_VAR} ${_generated} PARENT_SCOPE)
endfunction()
