# Copyright 2020-2026 Yonik Seeley and Luxir contributors
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.25)

# Native macOS builds use system/Homebrew libraries built for the host OS.
# Respect explicit deployment targets and toolchain-controlled builds.
if(CMAKE_HOST_APPLE AND NOT CMAKE_TOOLCHAIN_FILE
   AND NOT CMAKE_OSX_DEPLOYMENT_TARGET
   AND "$ENV{MACOSX_DEPLOYMENT_TARGET}" STREQUAL "")
  execute_process(COMMAND /usr/bin/sw_vers -productVersion
                  OUTPUT_VARIABLE _luxir_macos_version
                  OUTPUT_STRIP_TRAILING_WHITESPACE
                  COMMAND_ERROR_IS_FATAL ANY)
  if(_luxir_macos_version MATCHES "^([0-9]+\\.[0-9]+)")
    set(CMAKE_OSX_DEPLOYMENT_TARGET "${CMAKE_MATCH_1}" CACHE STRING
        "Minimum macOS version to target for deployment" FORCE)
  endif()
endif()
project(Luxir VERSION 0.1.0)
set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED True)

if(CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
  add_compile_options(-fexperimental-library)
  add_link_options(-fexperimental-library)
endif()

set(LUXIR_CPU_TARGET native CACHE STRING "Compiler CPU target for Luxir and vendored code")
set_property(CACHE LUXIR_CPU_TARGET PROPERTY STRINGS native apple-m1 x86-64-v2 x86-64-v3 x86-64-v4)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64|ARM64)$")
    if(NOT LUXIR_CPU_TARGET MATCHES "^(native|apple-m1)$")
      message(FATAL_ERROR "ARM64 builds require LUXIR_CPU_TARGET=native or apple-m1")
    endif()
    add_compile_options("-mcpu=${LUXIR_CPU_TARGET}")
  elseif(LUXIR_CPU_TARGET STREQUAL "native")
    add_compile_options(-march=native -mtune=native)
  else()
    add_compile_options("-march=${LUXIR_CPU_TARGET}" -mtune=generic)
  endif()
endif()

option(LUXIR_STATIC_RUNTIME "Link GCC runtimes statically, retaining dynamic glibc" OFF)
if(LUXIR_STATIC_RUNTIME)
  if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "LUXIR_STATIC_RUNTIME requires GCC on Linux")
  endif()
  add_link_options(-static-libstdc++ -static-libgcc -static-libasan -Wl,--as-needed)
  # FindOpenMP otherwise selects the compiler's shared libgomp.
  execute_process(COMMAND ${CMAKE_CXX_COMPILER} -print-file-name=libgomp.a
                  OUTPUT_VARIABLE _luxir_gomp OUTPUT_STRIP_TRAILING_WHITESPACE
                  COMMAND_ERROR_IS_FATAL ANY)
  if(NOT EXISTS "${_luxir_gomp}")
    message(FATAL_ERROR "The selected GCC does not supply libgomp.a")
  endif()
  set(OpenMP_gomp_LIBRARY "${_luxir_gomp}" CACHE FILEPATH "OpenMP runtime" FORCE)
endif()

option(LUXIR_PROBE_CONSTANT_HOOKS
       "Compile probe-constant same-binary measurement hooks" ON)
option(LUXIR_FULL_INLINE_DECODED_SUCCESSOR
       "Inline the complete decoded-successor SIMD scan" OFF)
option(LUXIR_DECODED_SUCCESSOR_HISTOGRAM
       "Count decoded-successor vector strides" OFF)
option(LUXIR_PROBE_WRAPPER_TRIMS
       "Enable scored-probe wrapper bookkeeping trims" OFF)
# C++26 "erroneous behavior" makes gcc zero-fill automatic storage by default,
# which taxes hot stack scratch buffers (measured ~35-40% on phrase queries).
# The presets override per-config flags: release/benchmarks builds use
# -ftrivial-auto-var-init=uninitialized (C++23 codegen); debug builds use
# =pattern so uninitialized reads surface as loud garbage instead of being
# masked by a plausible zero (zero-init also blinds valgrind/MSan detection).

# Build acceleration
# ccache: transparent compiler cache. No-op if ccache is not installed.
# pch_defines,time_macros sloppiness is required to get cache hits on TUs that
# use a precompiled header (see target_precompile_headers below). Apply it in
# the launcher rather than relying on each developer's global ccache config.
find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
  set(_LUXIR_CCACHE_LAUNCHER
      "${CMAKE_COMMAND};-E;env;CCACHE_SLOPPINESS=pch_defines,time_macros;${CCACHE_PROGRAM}")
  set(CMAKE_C_COMPILER_LAUNCHER   "${CCACHE_PROGRAM}")
  set(CMAKE_CXX_COMPILER_LAUNCHER "${_LUXIR_CCACHE_LAUNCHER}")
  message(STATUS "Luxir: ccache enabled (${CCACHE_PROGRAM})")
endif()

# Fast linker: prefer mold, then lld, then gold. Each candidate is verified with
# a real test link, so a linker that cannot handle this toolchain (e.g. mold
# 2.40 chokes on GCC 16's libatomic_asneeded.so AS_NEEDED script) is rejected
# automatically and we fall back to the next option, ending at the system
# default. NOTE: results are cached; after installing a new linker, reconfigure
# from a clean cache (or unset the LUXIR_LD_WORKS_* cache vars) to re-detect.
include(CheckLinkerFlag)
set(LUXIR_LINKER "")
foreach(_cand mold lld gold)
  check_linker_flag(CXX "-fuse-ld=${_cand}" LUXIR_LD_WORKS_${_cand})
  if(LUXIR_LD_WORKS_${_cand})
    add_link_options("-fuse-ld=${_cand}")
    set(LUXIR_LINKER "${_cand}")
    break()
  endif()
endforeach()
if(LUXIR_LINKER)
  message(STATUS "Luxir: using '${LUXIR_LINKER}' linker")
else()
  message(STATUS "Luxir: using default system linker (no faster linker usable)")
endif()

# This keeps dlls and executables together, which makes windows happy.
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

include_directories(SYSTEM ./src)

include(cmake/Dependencies.cmake)

include_directories(deps)

include(FetchContent)
set(LUXIR_FASTPFOR_DIR "${CMAKE_CURRENT_SOURCE_DIR}/deps/FastPFOR")
if(NOT EXISTS "${LUXIR_FASTPFOR_DIR}/headers/fastpfor_neon.h")
  FetchContent_Declare(luxir_fastpfor
    GIT_REPOSITORY https://github.com/fast-pack/FastPFOR.git
    GIT_TAG 2457e1ed1af35bbf7f4c509c863fa9797e637cb3
    SOURCE_SUBDIR luxir-unused-upstream-build)
  FetchContent_MakeAvailable(luxir_fastpfor)
  set(LUXIR_FASTPFOR_DIR "${luxir_fastpfor_SOURCE_DIR}")
endif()

# uni-algo (vendored under deps/uni-algo, Unicode 15.1.0): full Unicode case
# folding, NFC/NFKC, UAX#29 word/grapheme/sentence segmentation. The header API
# lives in deps/uni-algo/include; the Unicode tables compile once from
# src/data.cpp into this small static lib. Pinned + vendored on purpose - the
# analyzer's Unicode version is part of the index format (see the analysis
# design note). Conformance is gated by AnalysisTest before the real
# tokenizer/filter classes are built on top of it.
add_library(uni_algo STATIC deps/uni-algo/src/data.cpp)
target_include_directories(uni_algo PUBLIC deps/uni-algo/include)
set_target_properties(uni_algo PROPERTIES POSITION_INDEPENDENT_CODE ON)

# FastPFOR (vendored under deps/FastPFOR, v0.5.0): SIMD bit-packing kernels with
# native ARM NEON support (headers/fastpfor_neon.h). We compile the bit-packing
# sources the codecs use, plus streamvbyte.c for the experimental postings-tail
# SIMD varint path. We skip varintdecode.c and codecfactory.cpp because Luxir
# does not reference them.
add_library(fastpfor STATIC
  ${LUXIR_FASTPFOR_DIR}/src/bitpacking.cpp
  ${LUXIR_FASTPFOR_DIR}/src/simdbitpacking.cpp
  ${LUXIR_FASTPFOR_DIR}/src/simdunalignedbitpacking.cpp
  ${LUXIR_FASTPFOR_DIR}/src/streamvbyte.c)
target_include_directories(fastpfor SYSTEM PUBLIC "${LUXIR_FASTPFOR_DIR}/headers")
set_target_properties(fastpfor PROPERTIES POSITION_INDEPENDENT_CODE ON)

# set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-omit-frame-pointer")   #for profiling release
# sanitizer settings.. can leave this first one on while developing for linux
#set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++2b -g -O1 -fno-omit-frame-pointer -fsanitize=address")
#set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++2b -g -O0 -fno-omit-frame-pointer -fsanitize=address")     #don't optimize stuff out
#set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++2b -g -O0 -fno-omit-frame-pointer -fsanitize=address -DMEM_SCRIBBLE") #scribble over heap allocations before use
#set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++2b -g -O1 -fno-omit-frame-pointer -fsanitize=address -DMEMPOOL_MALLOC")  #use separate allocations
#set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++2b -g -O0 -fno-omit-frame-pointer -DMEMPOOL_MALLOC")  #use separate allocations, for valgrind
#set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++2b -g -O1 -fno-omit-frame-pointer -fsanitize=thread -fPIE -pie")
# NOTE: memory sanitizer needs *everything* including linked libc++ to be compiled with it.
# see https://github.com/google/sanitizers/wiki/MemorySanitizerLibcxxHowTo
#set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++2b -g -O1 -fno-omit-frame-pointer -fsanitize=memory -fsanitize-memory-track-origins -fPIE -pie -fno-optimize-sibling-calls")
#set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++2b -O3 -DNDEBUG -fstrict-aliasing")

message(STATUS "CMAKE_CXX_COMPILER_ID=${CMAKE_CXX_COMPILER_ID}")
if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  # Virtual overrides retain base signatures. NDEBUG also creates unused
  # assert-only variables, so warnings are fatal only in debug builds.
  add_compile_options(-Wall -Wextra -Wno-unused-parameter "$<$<CONFIG:DEBUG>:-Werror>")
elseif (CMAKE_CXX_COMPILER_ID STREQUAL "Intel")
elseif (CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -D__SSSE3__ -arch:AVX")
  add_compile_options(/W4)
  #if debugging?
  #add_link_options("/nodefaultlib:msvcrt")
endif()

# ---- Adaptive compile-parallelism CAP (memory-aware, Ninja job pool) ----
# hpp-proto template instantiation makes each heavy TU expensive to compile, so the build
# tool's default -j (logical cores: 32 on a hyperthreaded 16-core box) OOMs.
# This caps CONCURRENT COMPILES via a Ninja pool sized from THIS machine's physical cores
# and RAM, never above 16 (hyperthreads add no compile throughput past real cores). It is a
# cap, not a forced -j: a smaller -j still wins, and it adapts DOWN automatically on a
# smaller laptop. Override explicitly with -DLUXIR_COMPILE_JOBS=N. Tune _ram_jobs divisor
# (GB/job) down once the serializer is moved out-of-line and per-TU memory drops.
# Measured peak RSS of the heaviest single compile: 2.7G at -O0, 3.1G at -Og, 3.9G at
# -Og with ASan. The divisor covers the plain -Og case; an ASan build on a RAM-bound box
# uses a 4G/job divisor. Both account for the container memory limit.
# On a box with enough RAM per physical core the core count still binds.
if(NOT DEFINED LUXIR_COMPILE_JOBS)
  cmake_host_system_information(RESULT _ncores QUERY NUMBER_OF_PHYSICAL_CORES)
  cmake_host_system_information(RESULT _rammb  QUERY TOTAL_PHYSICAL_MEMORY)
  # Host memory may exceed Docker's allowance. Honor either cgroup version.
  foreach(_limit_file /sys/fs/cgroup/memory.max /sys/fs/cgroup/memory/memory.limit_in_bytes)
    if(EXISTS "${_limit_file}")
      file(READ "${_limit_file}" _memory_limit)
      string(STRIP "${_memory_limit}" _memory_limit)
      if(_memory_limit MATCHES "^[0-9]+$" AND _memory_limit LESS 9223372036854771712)
        math(EXPR _limit_mb "${_memory_limit} / 1048576")
        if(_limit_mb LESS _rammb)
          set(_rammb ${_limit_mb})
        endif()
      endif()
    endif()
  endforeach()
  if(CMAKE_CXX_FLAGS MATCHES "-fsanitize=address")
    set(_ram_per_job 4000)
  else()
    set(_ram_per_job 3000)
  endif()
  math(EXPR _ram_jobs "${_rammb} / ${_ram_per_job}")
  set(LUXIR_COMPILE_JOBS ${_ncores})
  if(_ram_jobs LESS LUXIR_COMPILE_JOBS)
    set(LUXIR_COMPILE_JOBS ${_ram_jobs})
  endif()
  if(LUXIR_COMPILE_JOBS GREATER 16)
    set(LUXIR_COMPILE_JOBS 16)
  endif()
  if(LUXIR_COMPILE_JOBS LESS 1)
    set(LUXIR_COMPILE_JOBS 1)
  endif()
  message(STATUS "Luxir compile-job cap: ${LUXIR_COMPILE_JOBS} (physical cores=${_ncores}, RAM=${_rammb}MB)")
else()
  message(STATUS "Luxir compile-job cap: ${LUXIR_COMPILE_JOBS} (configured)")
endif()
set_property(GLOBAL PROPERTY JOB_POOLS luxir_compile=${LUXIR_COMPILE_JOBS})
set(CMAKE_JOB_POOL_COMPILE luxir_compile)

# Include protobuf/grpc generated *.pb.h files
include_directories("${CMAKE_CURRENT_BINARY_DIR}")

# CONFIGURE_DEPENDS: re-glob on build so a newly added src .cpp is picked up without a manual
# reconfigure. Without it a new file is silently missing from luxir_lib in every build dir that
# has not been reconfigured, and only surfaces as an undefined symbol at link time - which is
# easy to miss when the preset you are iterating in happens to have been reconfigured already.
# (test/CMakeLists.txt has had this for the same reason.)
file(GLOB_RECURSE Proj_SOURCES CONFIGURE_DEPENDS src/*.cpp src/*.h)
list(FILTER Proj_SOURCES EXCLUDE REGEX .*/main.cpp)

add_library(luxir_lib STATIC ${Proj_SOURCES})
target_compile_definitions(luxir_lib PUBLIC
  # GCC -Og cannot honor xxHash's always_inline AVX-512 accumulator through
  # its function-pointer dispatch. Keep header inlining in optimized builds,
  # but use the linked implementation in Debug.
  LUXIR_XXHASH_INLINE=$<NOT:$<CONFIG:Debug>>
  LUXIR_PROBE_CONSTANT_HOOKS=$<BOOL:${LUXIR_PROBE_CONSTANT_HOOKS}>
  LUXIR_FULL_INLINE_DECODED_SUCCESSOR=$<BOOL:${LUXIR_FULL_INLINE_DECODED_SUCCESSOR}>
  LUXIR_DECODED_SUCCESSOR_HISTOGRAM=$<BOOL:${LUXIR_DECODED_SUCCESSOR_HISTOGRAM}>
  LUXIR_PROBE_WRAPPER_TRIMS=$<BOOL:${LUXIR_PROBE_WRAPPER_TRIMS}>)

add_executable(luxir src/luxir/main.cpp)
target_link_libraries(luxir luxir_lib)
set_source_files_properties(src/luxir/luxir_main.cpp PROPERTIES
  COMPILE_DEFINITIONS "LUXIR_VERSION=\"${PROJECT_VERSION}\";LUXIR_CPU_NAME=\"${LUXIR_CPU_TARGET}\"")
# Declare the executable's baseline, without marking optional dependency kernels
# as unconditional requirements. The loader can reject an unsupported CPU early.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_SYSTEM_NAME STREQUAL "Linux"
   AND LUXIR_CPU_TARGET MATCHES "^x86-64-v[234]$")
  target_compile_options(luxir PRIVATE -mneeded)
endif()

# Precompiled headers
# Heavy, rarely-changing external/generated headers shared across translation
# units. Project headers are intentionally excluded so editing them does not
# invalidate the PCH; the one exception is util/log.h, which is stable and is
# the only correct way to pull in spdlog (it sets SPDLOG_ACTIVE_LEVEL first).
# LUXIR_PCH_COMMON is consumed by the test target too, so it is set before
# add_subdirectory(test) and limited to headers on every target's include path.
set(LUXIR_PCH_COMMON
  <vector> <string> <string_view> <memory> <algorithm> <span>
  <optional> <unordered_map> <cstdint> <charconv>
  <boost/unordered/unordered_flat_map.hpp>
  # NOTE: the proto message header is intentionally NOT in the PCH during the
  # luxir::api cutover. A TU that includes the concrete header must not also pull
  # the trait-templated header (unqualified-count ADL clash), and converted TUs
  # use this PCH, so the proto header is included per-TU instead.
)
target_precompile_headers(luxir_lib PRIVATE
  ${LUXIR_PCH_COMMON}
  <oneapi/tbb/flow_graph.h>
  <oneapi/tbb/task_group.h>
  # Angle brackets: src/ is on the include path; quotes would resolve to the
  # wrong (src-less) source-relative path.
  <luxir/util/log.h>
)

# hpp-proto codegen toolchain (is_utf8, protoc-gen-hpp, hpp_proto::runtime,
# hpp_proto_generate()). Included unconditionally now that the engine migration to
# hpp luxir::proto is underway (big-bang). protos/ uses hpp_proto_generate().
include(cmake/hpp_proto.cmake)

add_subdirectory(protos)
add_subdirectory(test)

# luxir_descriptors.cpp #include "luxir_fds_data.h" (generated descriptor set); order its
# compile after generation. The PCH no longer force-includes any generated proto header.
add_dependencies(luxir_lib luxir_fds_gen)
# build/protos holds the generated luxir_fds_data.h (was provided via luxir_proto_hpp).
target_include_directories(luxir_lib PRIVATE "${CMAKE_BINARY_DIR}/protos")
# luxir_descriptors.cpp pulls protobuf descriptor headers; keep the hpp serializer
# PCH out of its TU (avoids the unqualified-count ADL clash, per the sweep split).
set_source_files_properties(src/luxir/server/luxir_descriptors.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)

if(ROBIN_HOOD_HASHING)
  find_path(ROBIN_HOOD_HASHING_INCLUDE_DIRS "robin_hood.h")
  target_include_directories(luxir_lib PRIVATE ${ROBIN_HOOD_HASHING_INCLUDE_DIRS})
endif()

target_include_directories(luxir_lib PUBLIC ${GTL_INCLUDE_DIRS})

target_link_libraries(luxir_lib PRIVATE TBB::tbb)
get_target_property(_luxir_tbb_type TBB::tbb TYPE)
if(_luxir_tbb_type STREQUAL "STATIC_LIBRARY")
  # Preserve TBB's internal allocator when its weak references cannot dlopen
  # tbbmalloc. This does not replace the application's malloc/free.
  target_link_libraries(luxir_lib PRIVATE TBB::tbbmalloc)
  target_link_options(luxir_lib INTERFACE
    -Wl,-u,scalable_malloc -Wl,-u,scalable_free
    -Wl,-u,scalable_aligned_malloc -Wl,-u,scalable_aligned_free)
endif()
target_link_libraries(luxir_lib PRIVATE xxHash::xxhash)
target_link_libraries(luxir_lib PUBLIC Boost::core Boost::thread Boost::sort)
# Codec.h exposes FastPFOR's 128-bit vector type to consumers.
target_link_libraries(luxir_lib PUBLIC fastpfor)
if(APPLE)
  target_link_libraries(luxir_lib PUBLIC date::date-tz)
endif()
# Concrete luxir::api wire model (post-cutover): hand-written concrete structs + out-of-line
# pb_meta/glz::meta (luxir_proto_concrete) carrying hpp_proto::runtime (is_utf8) + glaze.
# libprotobuf for google::protobuf::Arena (kept as the allocator) and the DescriptorPool used
# by descriptor-only reflection; grpc++ for the server. The trait-templated luxir_proto_hpp
# was dropped - no TU includes "protos/luxir*.pb.hpp" anymore. myproto (the protobuf message
# classes + typed stubs) is also intentionally gone.
target_link_libraries(luxir_lib PUBLIC luxir_proto_concrete)
target_link_libraries(luxir_lib PUBLIC protobuf::libprotobuf)
target_link_libraries(luxir_lib PUBLIC gRPC::grpc++ gRPC::grpc++_reflection)
# log.h is included by tests and other dependents; propagate spdlog's compiled
# mode so every TU shares the same default-logger registry.
target_link_libraries(luxir_lib PUBLIC spdlog::spdlog)
target_link_libraries(luxir_lib PRIVATE CLI11::CLI11)
target_link_libraries(luxir_lib PRIVATE lz4::lz4)
target_link_libraries(luxir_lib PRIVATE faiss)
# uni-algo (vendored): Unicode case folding / NFKC / UAX#29 segmentation, used by
# the analysis chain (src/luxir/analysis/Analyzer.cpp).
target_link_libraries(luxir_lib PRIVATE uni_algo)

# Optional, untracked developer-machine additions (extra tools/targets that
# should not live in the shared build). Opt-in: merely having the file present
# used to put its targets in `all`, so a box set up for bench runs paid for them
# on every ordinary build. Configure with -DLUXIR_LOCAL_TARGETS=ON to enable
# (cached per build dir, so a bench build dir sets it once). Targets the file
# adds should be EXCLUDE_FROM_ALL and built by name, so enabling it costs an
# ordinary build nothing.
option(LUXIR_LOCAL_TARGETS "Include the untracked LocalTargets.cmake" OFF)
if(LUXIR_LOCAL_TARGETS)
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/LocalTargets.cmake")
    message(STATUS "Luxir: including LocalTargets.cmake")
    include(LocalTargets.cmake)
  else()
    message(WARNING "LUXIR_LOCAL_TARGETS=ON but LocalTargets.cmake does not exist")
  endif()
endif()
