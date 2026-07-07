cmake_minimum_required(VERSION 3.16)
project(Solux VERSION 0.1)
set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED True)
# C++26 "erroneous behavior" makes gcc zero-fill automatic storage by default,
# which taxes hot stack scratch buffers (measured ~35-40% on phrase queries).
# The presets override per-config flags: release/benchmarks builds use
# -ftrivial-auto-var-init=uninitialized (C++23 codegen); debug builds use
# =pattern so uninitialized reads surface as loud garbage instead of being
# masked by a plausible zero (zero-init also blinds valgrind/MSan detection).

# Build acceleration
# ccache: transparent compiler cache. No-op if ccache is not installed.
# pch_defines,time_macros sloppiness is required to get cache hits on TUs that
# use a precompiled header (see target_precompile_headers below).
find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
  set(CMAKE_C_COMPILER_LAUNCHER   "${CCACHE_PROGRAM}")
  set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
  message(STATUS "Solux: ccache enabled (${CCACHE_PROGRAM})")
endif()

# Fast linker: prefer mold, then lld, then gold. Each candidate is verified with
# a real test link, so a linker that cannot handle this toolchain (e.g. mold
# 2.40 chokes on GCC 16's libatomic_asneeded.so AS_NEEDED script) is rejected
# automatically and we fall back to the next option, ending at the system
# default. NOTE: results are cached; after installing a new linker, reconfigure
# from a clean cache (or unset the SOLUX_LD_WORKS_* cache vars) to re-detect.
include(CheckLinkerFlag)
set(SOLUX_LINKER "")
foreach(_cand mold lld gold)
  check_linker_flag(CXX "-fuse-ld=${_cand}" SOLUX_LD_WORKS_${_cand})
  if(SOLUX_LD_WORKS_${_cand})
    add_link_options("-fuse-ld=${_cand}")
    set(SOLUX_LINKER "${_cand}")
    break()
  endif()
endforeach()
if(SOLUX_LINKER)
  message(STATUS "Solux: using '${SOLUX_LINKER}' linker")
else()
  message(STATUS "Solux: using default system linker (no faster linker usable)")
endif()

# This keeps dlls and executables together, which makes windows happy.
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

include_directories(SYSTEM ./src)

find_package(xxHash CONFIG REQUIRED)

#from clion docs
find_package(Boost)
IF (Boost_FOUND)
    include_directories(${Boost_INCLUDE_DIR})
endif()
set (Boost_USE_STATIC_LIBS OFF) # enable dynamic linking
set (Boost_USE_MULTITHREAD ON)  # enable multithreading
find_package (Boost COMPONENTS REQUIRED core thread sort)

find_package(TBB CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(CLI11 CONFIG REQUIRED)
find_package(lz4 CONFIG REQUIRED)
# JSON (both the proto-JSON dialect and the HTTP envelope) is glaze, which comes in
# via solux_proto_concrete. Boost.Beast/Asio (the HTTP transport) are header-only and
# come with the Boost include dir found above.
# FAISS for vector ANN aux indexes (gcc vcpkg presets only).
# FAISS exports OpenMP::OpenMP_CXX in its imported-target interface, so we must
# locate OpenMP before find_package(faiss).
find_package(OpenMP REQUIRED)
find_package(faiss CONFIG REQUIRED)
# protobuf (DescriptorPool + google::protobuf::Arena) and gRPC are linked directly into
# solux_lib now that myproto is gone; they used to arrive transitively via myproto.
find_package(Protobuf CONFIG REQUIRED)
find_package(gRPC CONFIG REQUIRED)

include_directories(deps)

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
# SIMD varint path. We skip varintdecode.c and codecfactory.cpp because Solux
# does not reference them.
add_library(fastpfor STATIC
  deps/FastPFOR/src/bitpacking.cpp
  deps/FastPFOR/src/simdbitpacking.cpp
  deps/FastPFOR/src/simdunalignedbitpacking.cpp
  deps/FastPFOR/src/streamvbyte.c)
target_include_directories(fastpfor PUBLIC deps/FastPFOR/headers)
set_target_properties(fastpfor PROPERTIES POSITION_INDEPENDENT_CODE ON)
# streamvbyte.c is C, so CMAKE_CXX_FLAGS does not reach it. Set the arch flags
# on the target so its SSSE3/SSE4.1 intrinsics are enabled for both C and C++.
target_compile_options(fastpfor PRIVATE -march=native -mtune=native)

#Print out all env vars... useful while build system is in development
get_cmake_property(_variableNames VARIABLES)
list (SORT _variableNames)
foreach (_variableName ${_variableNames})
    message(STATUS "${_variableName}=${${_variableName}}")
endforeach()

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
if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -mavx -mssse3 -march=native -mtune=native")
  add_compile_options(
          "$<$<CONFIG:RELEASE>:-g1>"
#          "$<$<CONFIG:DEBUG>:-D_GLIBCXX_DEBUG>"
  )
  # -Wno-unused-parameter: many virtual overrides must keep base-class signature; renaming params would just be noise.
  add_compile_options(-Wall -Wextra -Wno-unused-parameter)
elseif (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -mavx -mssse3 -march=native -mtune=native")   # native just for dev... lock down more later?
  add_compile_options(
          "$<$<CONFIG:RELEASE>:-g1>"
#          "$<$<CONFIG:DEBUG>:-D_GLIBCXX_DEBUG>"
  )
  add_compile_options(-Wall -Wextra -Wno-unused-parameter)
elseif (CMAKE_CXX_COMPILER_ID STREQUAL "Intel")
elseif (CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -D__SSSE3__ -arch:AVX")
  add_compile_options(/W4)
  #if debugging?
  #add_link_options("/nodefaultlib:msvcrt")
endif()

# ---- Adaptive compile-parallelism CAP (memory-aware, Ninja job pool) ----
# hpp-proto template instantiation makes each heavy TU cost ~1.5-2.5G RAM to compile, so
# the build tool's default -j (logical cores: 32 on a hyperthreaded 16-core box) OOMs.
# This caps CONCURRENT COMPILES via a Ninja pool sized from THIS machine's physical cores
# and RAM, never above 16 (hyperthreads add no compile throughput past real cores). It is a
# cap, not a forced -j: a smaller -j still wins, and it adapts DOWN automatically on a
# smaller laptop. Override explicitly with -DSOLUX_COMPILE_JOBS=N. Tune _ram_jobs divisor
# (GB/job) down once the serializer is moved out-of-line and per-TU memory drops.
if(NOT DEFINED SOLUX_COMPILE_JOBS)
  cmake_host_system_information(RESULT _ncores QUERY NUMBER_OF_PHYSICAL_CORES)
  cmake_host_system_information(RESULT _rammb  QUERY TOTAL_PHYSICAL_MEMORY)
  math(EXPR _ram_jobs "${_rammb} / 2000")   # ~2.0G per concurrent heavy compile (post serializer-isolation ceiling)
  set(SOLUX_COMPILE_JOBS ${_ncores})
  if(_ram_jobs LESS SOLUX_COMPILE_JOBS)
    set(SOLUX_COMPILE_JOBS ${_ram_jobs})
  endif()
  if(SOLUX_COMPILE_JOBS GREATER 16)
    set(SOLUX_COMPILE_JOBS 16)
  endif()
  if(SOLUX_COMPILE_JOBS LESS 1)
    set(SOLUX_COMPILE_JOBS 1)
  endif()
endif()
message(STATUS "Solux compile-job cap: ${SOLUX_COMPILE_JOBS} (physical cores=${_ncores}, RAM=${_rammb}MB)")
set_property(GLOBAL PROPERTY JOB_POOLS solux_compile=${SOLUX_COMPILE_JOBS})
set(CMAKE_JOB_POOL_COMPILE solux_compile)

# Include protobuf/grpc generated *.pb.h files
include_directories("${CMAKE_CURRENT_BINARY_DIR}")

file(GLOB_RECURSE Proj_SOURCES src/*.cpp src/*.h)
list(FILTER Proj_SOURCES EXCLUDE REGEX .*/main.cpp)

add_library(solux_lib STATIC ${Proj_SOURCES})

add_executable(solux src/solux/main.cpp)
target_link_libraries(solux solux_lib)

# Precompiled headers
# Heavy, rarely-changing external/generated headers shared across translation
# units. Project headers are intentionally excluded so editing them does not
# invalidate the PCH; the one exception is util/log.h, which is stable and is
# the only correct way to pull in spdlog (it sets SPDLOG_ACTIVE_LEVEL first).
# SOLUX_PCH_COMMON is consumed by the test target too, so it is set before
# add_subdirectory(test) and limited to headers on every target's include path.
set(SOLUX_PCH_COMMON
  <vector> <string> <string_view> <memory> <algorithm> <span>
  <optional> <unordered_map> <cstdint> <charconv>
  <boost/unordered/unordered_flat_map.hpp>
  # NOTE: the proto message header is intentionally NOT in the PCH during the
  # solux::api cutover. A TU that includes the concrete header must not also pull
  # the trait-templated header (unqualified-count ADL clash), and converted TUs
  # use this PCH, so the proto header is included per-TU instead.
)
target_precompile_headers(solux_lib PRIVATE
  ${SOLUX_PCH_COMMON}
  <oneapi/tbb/flow_graph.h>
  <oneapi/tbb/task_group.h>
  # Angle brackets: src/ is on the include path; quotes would resolve to the
  # wrong (src-less) source-relative path.
  <solux/util/log.h>
)

# hpp-proto codegen toolchain (is_utf8, protoc-gen-hpp, hpp_proto::runtime,
# hpp_proto_generate()). Included unconditionally now that the engine migration to
# hpp solux::proto is underway (big-bang). protos/ uses hpp_proto_generate().
include(cmake/hpp_proto.cmake)

add_subdirectory(protos)
add_subdirectory(test)

# solux_descriptors.cpp #include "solux_fds_data.h" (generated descriptor set); order its
# compile after generation. The PCH no longer force-includes any generated proto header.
add_dependencies(solux_lib solux_fds_gen)
# build/protos holds the generated solux_fds_data.h (was provided via solux_proto_hpp).
target_include_directories(solux_lib PRIVATE "${CMAKE_BINARY_DIR}/protos")
# solux_descriptors.cpp pulls protobuf descriptor headers; keep the hpp serializer
# PCH out of its TU (avoids the unqualified-count ADL clash, per the sweep split).
set_source_files_properties(src/solux/server/solux_descriptors.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)

if(ROBIN_HOOD_HASHING)
  find_path(ROBIN_HOOD_HASHING_INCLUDE_DIRS "robin_hood.h")
  target_include_directories(solux_lib PRIVATE ${ROBIN_HOOD_HASHING_INCLUDE_DIRS})
endif()

find_path(GTL_INCLUDE_DIRS "gtl/bit_vector.hpp")
target_include_directories(solux_lib PRIVATE ${GTL_INCLUDE_DIRS})

target_link_libraries(solux_lib PRIVATE TBB::tbb)
target_link_libraries(solux_lib PRIVATE xxHash::xxhash)
target_link_libraries(solux_lib PRIVATE ${Boost_LIBRARIES})
target_link_libraries(solux_lib PRIVATE fastpfor)
# Concrete solux::api wire model (post-cutover): hand-written concrete structs + out-of-line
# pb_meta/glz::meta (solux_proto_concrete) carrying hpp_proto::runtime (is_utf8) + glaze.
# libprotobuf for google::protobuf::Arena (kept as the allocator) and the DescriptorPool used
# by descriptor-only reflection; grpc++ for the server. The trait-templated solux_proto_hpp
# was dropped - no TU includes "protos/solux*.pb.hpp" anymore. myproto (the protobuf message
# classes + typed stubs) is also intentionally gone.
target_link_libraries(solux_lib PUBLIC solux_proto_concrete)
target_link_libraries(solux_lib PUBLIC protobuf::libprotobuf)
target_link_libraries(solux_lib PUBLIC gRPC::grpc++ gRPC::grpc++_reflection)
# log.h is included by tests and other dependents; propagate spdlog's compiled
# mode so every TU shares the same default-logger registry.
target_link_libraries(solux_lib PUBLIC spdlog::spdlog)
target_link_libraries(solux_lib PRIVATE CLI11::CLI11)
target_link_libraries(solux_lib PRIVATE lz4::lz4)
target_link_libraries(solux_lib PRIVATE faiss)
# uni-algo (vendored): Unicode case folding / NFKC / UAX#29 segmentation, used by
# the analysis chain (src/solux/analysis/Analyzer.cpp).
target_link_libraries(solux_lib PRIVATE uni_algo)

# Optional, untracked developer-machine additions (extra tools/targets that
# should not live in the shared build). Silently skipped when absent.
include(LocalTargets.cmake OPTIONAL)
