# Copyright 2020-2026 Yonik Seeley and Luxir contributors
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.16)

# The native presets name a dependency variant; its vcpkg triplets depend on the
# host (see deps/make_deps.sh). Set before project() loads the vcpkg toolchain.
if(DEFINED LUXIR_NATIVE_DEPS AND NOT DEFINED VCPKG_TARGET_TRIPLET)
  if(CMAKE_HOST_APPLE)
    set(_luxir_triplet arm64-osx-luxir-m1)
  else()
    set(_luxir_triplet x64-linux-luxir-native)
  endif()
  if(LUXIR_NATIVE_DEPS STREQUAL "native")
    set(_luxir_host_triplet ${_luxir_triplet})
  elseif(LUXIR_NATIVE_DEPS STREQUAL "native-asan")
    set(_luxir_host_triplet ${_luxir_triplet}-host)
    string(APPEND _luxir_triplet -asan)
  else()
    message(FATAL_ERROR "LUXIR_NATIVE_DEPS must be native or native-asan")
  endif()
  set(VCPKG_TARGET_TRIPLET ${_luxir_triplet} CACHE STRING "")
  set(VCPKG_HOST_TRIPLET ${_luxir_host_triplet} CACHE STRING "")
endif()

project(Luxir VERSION 0.1.0)
set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED True)

set(LUXIR_CPU_TARGET native CACHE STRING "Compiler CPU target for Luxir and vendored code")
set_property(CACHE LUXIR_CPU_TARGET PROPERTY STRINGS native x86-64-v2 x86-64-v3 x86-64-v4 armv8-a
             apple-m1 apple-m2 apple-m3 apple-m4 apple-m5)
set(_luxir_cpu_name ${LUXIR_CPU_TARGET})
# GCC's native detection reports every Apple core as apple-m1; name the chip.
if(LUXIR_CPU_TARGET STREQUAL "native" AND APPLE AND CMAKE_SYSTEM_PROCESSOR STREQUAL "arm64")
  execute_process(COMMAND sysctl -n machdep.cpu.brand_string
                  OUTPUT_VARIABLE _luxir_brand OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(_luxir_brand MATCHES "^Apple (M[0-9]+)")
    string(TOLOWER "apple-${CMAKE_MATCH_1}" _luxir_cpu_name)
    include(CheckCXXCompilerFlag)
    check_cxx_compiler_flag("-mcpu=${_luxir_cpu_name}" LUXIR_MCPU_${CMAKE_MATCH_1})
    if(NOT LUXIR_MCPU_${CMAKE_MATCH_1})
      message(WARNING "${CMAKE_CXX_COMPILER} does not know -mcpu=${_luxir_cpu_name}; "
                      "using -march=native")
      set(_luxir_cpu_name native)
    endif()
  endif()
endif()
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  if(_luxir_cpu_name MATCHES "^apple-")
    # An Apple core name selects both the architecture and the tuning.
    add_compile_options("-mcpu=${_luxir_cpu_name}")
  else()
    if(LUXIR_CPU_TARGET STREQUAL "native")
      set(_luxir_cpu_tune native)
    else()
      set(_luxir_cpu_tune generic)
    endif()
    add_compile_options("-march=${_luxir_cpu_name}" "-mtune=${_luxir_cpu_tune}")
  endif()
  # Plain -fstack-protector instead of the distro-default -strong. -strong also
  # instruments functions with any array or address-taken local, putting canary
  # loads in per-document loops (up to 9% on facet loops). The canary lives at
  # a fixed page offset in the thread's TCB, so pushes in a hot frame can
  # 4K-alias it and move timing by ~3% on unrelated frame-size changes. Plain
  # still covers functions with character buffers, where overflows of request
  # bytes would occur. Index-data hot paths that keep one opt out with
  # LUXIR_NO_STACK_PROTECTOR.
  add_compile_options(-fstack-protector)
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

# Sanitizers replace malloc; default to jemalloc only without one.
string(TOUPPER "${CMAKE_BUILD_TYPE}" _luxir_config)
set(_luxir_build_flags " ${CMAKE_C_FLAGS} ${CMAKE_CXX_FLAGS} ${CMAKE_EXE_LINKER_FLAGS} ")
string(APPEND _luxir_build_flags
       "${CMAKE_C_FLAGS_${_luxir_config}} ${CMAKE_CXX_FLAGS_${_luxir_config}} "
       "${CMAKE_EXE_LINKER_FLAGS_${_luxir_config}} ")
set(_luxir_sanitizer "")
# jemalloc is not yet supported on macOS (deps/make_deps.sh omits it there).
set(_luxir_jemalloc_default ON)
if(APPLE)
  set(_luxir_jemalloc_default OFF)
endif()
if(_luxir_build_flags MATCHES
   " -fsanitize=([a-z,-]*,)?(address|hwaddress|thread|memory|leak)[ ,]")
  set(_luxir_sanitizer "${CMAKE_MATCH_2}")
  set(_luxir_jemalloc_default OFF)
endif()
# Valgrind needs --soname-synonyms=somalloc=NONE for static jemalloc, or OFF.
option(LUXIR_JEMALLOC "Link jemalloc as the process allocator" ${_luxir_jemalloc_default})
if(LUXIR_JEMALLOC AND _luxir_sanitizer)
  message(FATAL_ERROR "LUXIR_JEMALLOC=ON conflicts with -fsanitize=${_luxir_sanitizer}, "
                      "which replaces malloc. Configure with -DLUXIR_JEMALLOC=OFF.")
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
# ---- Build parallelism: compile limit sized from this machine ----
# hpp-proto template instantiation makes each heavy TU expensive to compile, so the build
# tool's default -j (logical cores) can exhaust memory. tools/build-jobs sizes the limit
# from physical cores and memory (cgroup limits included): at most one compile per core,
# never above 16 (hyperthreads add little compile throughput). Measured peak RSS of the
# heaviest single compile: 2.7G at -O0, 3.1G at -Og, 3.9G at -Og with ASan. Links need no
# limit: mold links luxir_test in under 2s with about 1.4G of anonymous memory. Override
# with -DLUXIR_COMPILE_JOBS=N. It is a cap, not a forced -j: a smaller -j still wins.
if(NOT DEFINED LUXIR_COMPILE_JOBS)
  if(_luxir_sanitizer)
    set(_luxir_compile_mb 4000)
  else()
    set(_luxir_compile_mb 3000)
  endif()
  execute_process(COMMAND "${CMAKE_SOURCE_DIR}/tools/build-jobs" ${_luxir_compile_mb}
                  OUTPUT_VARIABLE LUXIR_COMPILE_JOBS OUTPUT_STRIP_TRAILING_WHITESPACE
                  RESULT_VARIABLE _luxir_jobs_result ERROR_QUIET)
  if(NOT _luxir_jobs_result EQUAL 0 OR NOT LUXIR_COMPILE_JOBS MATCHES "^[0-9]+$")
    message(WARNING "tools/build-jobs failed; assuming 4 compile jobs")
    set(LUXIR_COMPILE_JOBS 4)
  endif()
  if(LUXIR_COMPILE_JOBS GREATER 16)
    set(LUXIR_COMPILE_JOBS 16)
  endif()
endif()
message(STATUS "Luxir compile-job cap: ${LUXIR_COMPILE_JOBS}")
set_property(GLOBAL PROPERTY JOB_POOLS luxir_compile=${LUXIR_COMPILE_JOBS})
set(CMAKE_JOB_POOL_COMPILE luxir_compile)

# The Ninja pool only sees this build. Compiles also hold one of LUXIR_COMPILE_JOBS
# machine-wide lock files (tools/build-slot), so concurrent builds from other worktrees,
# presets, or the development container share the limit instead of each assuming the
# whole machine. Set LUXIR_BUILD_SLOT_DIR to an empty string to limit this build alone.
if(DEFINED ENV{LUXIR_CACHE_DIR})
  set(_luxir_cache_dir "$ENV{LUXIR_CACHE_DIR}")
elseif(DEFINED ENV{XDG_CACHE_HOME})
  set(_luxir_cache_dir "$ENV{XDG_CACHE_HOME}/luxir")
else()
  set(_luxir_cache_dir "$ENV{HOME}/.cache/luxir")
endif()
set(LUXIR_BUILD_SLOT_DIR "${_luxir_cache_dir}/build-slots" CACHE PATH
    "Directory of machine-wide build slot locks shared by concurrent builds; empty disables")
find_program(LUXIR_FLOCK flock)
set(_luxir_compile_launcher "")
if(LUXIR_BUILD_SLOT_DIR AND LUXIR_FLOCK)
  file(MAKE_DIRECTORY "${LUXIR_BUILD_SLOT_DIR}")
  set(_luxir_compile_launcher
      "${CMAKE_SOURCE_DIR}/tools/build-slot;${LUXIR_BUILD_SLOT_DIR}/compile;${LUXIR_COMPILE_JOBS}")
  message(STATUS "Luxir: build slots shared through ${LUXIR_BUILD_SLOT_DIR}")
endif()

# ccache: transparent compiler cache. No-op if ccache is not installed.
# pch_defines,time_macros sloppiness is required to get cache hits on TUs that
# use a precompiled header (see target_precompile_headers below). Apply it in
# the launcher rather than relying on each developer's global ccache config.
# tools/ccache-launch also deletes a stale .gch before the PCH compile, without
# which ccache never caches a rebuilt PCH.
# LUXIR_CCACHE_RELATIVE_PATHS shares objects between checkouts and worktrees at different
# paths: CCACHE_BASEDIR makes the paths ccache hashes relative, and the prefix map makes
# the debug info's compilation directory relative to the checkout. PWD gives ccache the
# build directory as configured (Ninja runs every compile there, and ccache ignores a PWD
# naming another directory), so the prefix map still applies to a checkout reached through
# a symlink. Relative paths only match between checkouts when the build directory is
# physically inside its checkout. It is off by default because compiler diagnostics and
# __FILE__ then name sources relative to the build directory, and debuggers resolve
# sources from the checkout root.
option(LUXIR_CCACHE_RELATIVE_PATHS "Use checkout-relative paths so ccache shares objects across checkouts" OFF)
find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
  list(APPEND _luxir_compile_launcher "${CMAKE_SOURCE_DIR}/tools/ccache-launch" CCACHE_SLOPPINESS=pch_defines,time_macros)
  if(LUXIR_CCACHE_RELATIVE_PATHS)
    list(APPEND _luxir_compile_launcher "CCACHE_BASEDIR=${CMAKE_SOURCE_DIR}" "PWD=${CMAKE_BINARY_DIR}")
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
      add_compile_options("-fdebug-prefix-map=${CMAKE_SOURCE_DIR}=.")
    endif()
    file(REAL_PATH "${CMAKE_SOURCE_DIR}" _luxir_real_source)
    file(REAL_PATH "${CMAKE_BINARY_DIR}" _luxir_real_binary)
    cmake_path(IS_PREFIX _luxir_real_source "${_luxir_real_binary}" _luxir_binary_inside)
    if(NOT _luxir_binary_inside)
      message(WARNING "Luxir: build directory ${_luxir_real_binary} is not inside ${_luxir_real_source}, "
                      "so ccache cannot share its objects with other checkouts")
    endif()
  endif()
  list(APPEND _luxir_compile_launcher "${CCACHE_PROGRAM}")
  message(STATUS "Luxir: ccache enabled (${CCACHE_PROGRAM})")
endif()
set(CMAKE_C_COMPILER_LAUNCHER   "${_luxir_compile_launcher}")
set(CMAKE_CXX_COMPILER_LAUNCHER "${_luxir_compile_launcher}")

# Fast linker: prefer mold, then lld, then gold. Each candidate is verified with
# a real test link, so a linker that cannot handle this toolchain (e.g. mold
# 2.40 chokes on GCC 16's libatomic_asneeded.so AS_NEEDED script) is rejected
# automatically and we fall back to the next option, ending at the system
# default. NOTE: results are cached; after installing a new linker, reconfigure
# from a clean cache (or unset the LUXIR_LD_WORKS_* cache vars) to re-detect.
#
# mold before 2.31.0 is skipped. When input sections share a name but differ
# in flags, it ORs in the flags of only the first such section each worker
# thread sees, so the output flags depend on thread scheduling (fixed upstream
# in 6463a7c). upb's linkarr_upb_AllExts mixes a read-only sentinel with
# writable extension entries, so some links put the entries in a read-only
# segment and the binary segfaults in ld.so before main(). A test link cannot
# catch a race, so check the version. It is not cached, so old caches get it.
#
# GCC on darwin accepts any of these -fuse-ld values but always runs the
# system linker, so the probe would only mislabel it.
include(CheckLinkerFlag)
set(LUXIR_LINKER "")
# Cross builds use their toolchain's linker. A host linker can pass the small
# probe but mishandle target relocations: mold placed ARM64 upb extension
# pointers in a read-only segment, causing a loader crash before main().
if(NOT CMAKE_CROSSCOMPILING AND NOT APPLE)
  foreach(_cand mold lld gold)
    check_linker_flag(CXX "-fuse-ld=${_cand}" LUXIR_LD_WORKS_${_cand})
    if(LUXIR_LD_WORKS_${_cand} AND _cand STREQUAL "mold")
      execute_process(
        COMMAND "${CMAKE_CXX_COMPILER}" -fuse-ld=mold -Wl,--version
        OUTPUT_VARIABLE _luxir_mold_out ERROR_QUIET)
      string(REGEX MATCH "mold ([0-9]+\\.[0-9]+(\\.[0-9]+)?)" _ "${_luxir_mold_out}")
      set(_luxir_mold_version "${CMAKE_MATCH_1}")
      if(NOT _luxir_mold_version OR _luxir_mold_version VERSION_LESS 2.31.0)
        message(STATUS "Luxir: skipping mold ${_luxir_mold_version} "
                       "(before 2.31.0 its output section flags are nondeterministic)")
        continue()
      endif()
    endif()
    if(LUXIR_LD_WORKS_${_cand})
      add_link_options("-fuse-ld=${_cand}")
      set(LUXIR_LINKER "${_cand}")
      break()
    endif()
  endforeach()
endif()
if(LUXIR_LINKER)
  message(STATUS "Luxir: using '${LUXIR_LINKER}' linker")
elseif(CMAKE_CROSSCOMPILING)
  message(STATUS "Luxir: using the cross-toolchain linker")
else()
  message(STATUS "Luxir: using default system linker (no faster linker usable)")
endif()

# A Mach-O executable exports every external symbol, and GCC makes each inline
# function and template instance a weak one, so dyld coalesced about 200k weak
# definitions at every launch (0.5 s for luxir). Export only main, as an ELF
# executable does without -rdynamic. Unstripped stack traces keep their names.
if(APPLE)
  string(APPEND CMAKE_EXE_LINKER_FLAGS " -Wl,-exported_symbol,_main")
endif()

# Thin static archives: the project's libraries only feed this build's own
# links, so an archive can index its objects in place instead of copying them.
# Rewriting the full luxir_lib archive after each engine edit took seconds.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  foreach(_lang C CXX)
    set(CMAKE_${_lang}_ARCHIVE_CREATE "<CMAKE_AR> qcT <TARGET> <LINK_FLAGS> <OBJECTS>")
    set(CMAKE_${_lang}_ARCHIVE_APPEND "<CMAKE_AR> qT <TARGET> <LINK_FLAGS> <OBJECTS>")
  endforeach()
endif()

# This keeps dlls and executables together, which makes windows happy.
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

include_directories(SYSTEM ./src)

find_package(xxHash CONFIG REQUIRED)

#from clion docs
find_package(Boost CONFIG REQUIRED COMPONENTS core thread sort)

find_package(TBB CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(CLI11 CONFIG REQUIRED)
find_package(lz4 CONFIG REQUIRED)
find_package(zstd CONFIG REQUIRED)
# JSON (both the proto-JSON dialect and the HTTP envelope) is glaze, which comes in
# via luxir_proto_concrete. Boost.Beast/Asio (the HTTP transport) are header-only and
# come with the Boost include dir found above.
# FAISS for vector ANN aux indexes (gcc vcpkg presets only).
# FAISS exports OpenMP::OpenMP_CXX in its imported-target interface, so we must
# locate OpenMP before find_package(faiss).
find_package(OpenMP REQUIRED)
find_package(faiss CONFIG REQUIRED)
# protobuf (DescriptorPool + google::protobuf::Arena) and gRPC are linked directly into
# luxir_lib now that myproto is gone; they used to arrive transitively via myproto.
find_package(Protobuf CONFIG REQUIRED)
find_package(gRPC CONFIG REQUIRED)
find_package(absl CONFIG REQUIRED)
if(LUXIR_JEMALLOC)
  # The vcpkg port installs a static archive and pkg-config metadata only.
  set(_luxir_triplet_dir "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}")
  find_path(LUXIR_JEMALLOC_INCLUDE_DIR jemalloc/jemalloc.h
    PATHS "${_luxir_triplet_dir}/include" NO_DEFAULT_PATH)
  find_library(LUXIR_JEMALLOC_RELEASE_LIBRARY NAMES libjemalloc.a
    PATHS "${_luxir_triplet_dir}/lib" NO_DEFAULT_PATH)
  find_library(LUXIR_JEMALLOC_DEBUG_LIBRARY NAMES libjemalloc.a
    PATHS "${_luxir_triplet_dir}/debug/lib" NO_DEFAULT_PATH)
  if(_luxir_config STREQUAL "DEBUG")
    set(_luxir_jemalloc_library "${LUXIR_JEMALLOC_DEBUG_LIBRARY}")
  else()
    set(_luxir_jemalloc_library "${LUXIR_JEMALLOC_RELEASE_LIBRARY}")
  endif()
  if(NOT LUXIR_JEMALLOC_INCLUDE_DIR OR NOT _luxir_jemalloc_library)
    message(FATAL_ERROR "LUXIR_JEMALLOC=ON requires the jemalloc feature of deps/vcpkg.json "
                        "(deps/make_deps.sh installs it). Or configure with -DLUXIR_JEMALLOC=OFF.")
  endif()
  find_package(Threads REQUIRED)
  add_library(luxir_jemalloc STATIC IMPORTED)
  set_target_properties(luxir_jemalloc PROPERTIES
    IMPORTED_CONFIGURATIONS "DEBUG;RELEASE"
    IMPORTED_LOCATION_DEBUG "${LUXIR_JEMALLOC_DEBUG_LIBRARY}"
    IMPORTED_LOCATION_RELEASE "${LUXIR_JEMALLOC_RELEASE_LIBRARY}"
    MAP_IMPORTED_CONFIG_RELWITHDEBINFO Release
    MAP_IMPORTED_CONFIG_MINSIZEREL Release
    INTERFACE_INCLUDE_DIRECTORIES "${LUXIR_JEMALLOC_INCLUDE_DIR}"
    INTERFACE_LINK_LIBRARIES "Threads::Threads;m;${CMAKE_DL_LIBS}")
endif()

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

# FastPFOR (vendored under deps/FastPFOR, v0.5.0): SIMD bit-packing kernels with
# native ARM NEON support (headers/fastpfor_neon.h). We compile the bit-packing
# sources the codecs use, plus streamvbyte.c for the experimental postings-tail
# SIMD varint path. We skip varintdecode.c and codecfactory.cpp because Luxir
# does not reference them.
add_library(fastpfor STATIC
  deps/FastPFOR/src/bitpacking.cpp
  deps/FastPFOR/src/simdbitpacking.cpp
  deps/FastPFOR/src/simdunalignedbitpacking.cpp
  deps/FastPFOR/src/streamvbyte.c)
target_include_directories(fastpfor PUBLIC deps/FastPFOR/headers)

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

# Include protobuf/grpc generated *.pb.h files
include_directories("${CMAKE_CURRENT_BINARY_DIR}")

# CONFIGURE_DEPENDS: re-glob on build so a newly added src .cpp is picked up without a manual
# reconfigure. Without it a new file is silently missing from luxir_lib in every build dir that
# has not been reconfigured, and only surfaces as an undefined symbol at link time - which is
# easy to miss when the preset you are iterating in happens to have been reconfigured already.
# (test/CMakeLists.txt has had this for the same reason.)
file(GLOB_RECURSE Proj_SOURCES CONFIGURE_DEPENDS src/*.cpp src/*.h)
list(FILTER Proj_SOURCES EXCLUDE REGEX .*/main.cpp)

include(cmake/BuildInfo.cmake)
add_library(luxir_lib STATIC ${Proj_SOURCES} "${CMAKE_CURRENT_BINARY_DIR}/BuildInfo.cpp")
target_compile_definitions(luxir_lib PUBLIC
  # oneTBB preview API for task_arena::leave_policy (LuxirNode). Global so
  # every translation unit, and the precompiled headers that include
  # task_arena.h, sees the same TBB declarations.
  TBB_PREVIEW_PARALLEL_PHASE=1
  # GCC -Og cannot honor xxHash's always_inline AVX-512 accumulator through
  # its function-pointer dispatch. Keep header inlining in optimized builds,
  # but use the linked implementation in Debug.
  LUXIR_XXHASH_INLINE=$<NOT:$<CONFIG:Debug>>
  LUXIR_PROBE_CONSTANT_HOOKS=$<BOOL:${LUXIR_PROBE_CONSTANT_HOOKS}>
  LUXIR_FULL_INLINE_DECODED_SUCCESSOR=$<BOOL:${LUXIR_FULL_INLINE_DECODED_SUCCESSOR}>
  LUXIR_DECODED_SUCCESSOR_HISTOGRAM=$<BOOL:${LUXIR_DECODED_SUCCESSOR_HISTOGRAM}>
  LUXIR_PROBE_WRAPPER_TRIMS=$<BOOL:${LUXIR_PROBE_WRAPPER_TRIMS}>)

add_executable(luxir src/luxir/main.cpp)
target_link_libraries(luxir PRIVATE luxir_lib)
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

# Compiled-in plugins (docs/dev/plugins.md). LUXIR_PLUGIN_DIRS lists directories whose
# CMakeLists.txt call luxir_add_plugin; the in-tree test-only example is always added.
set(LUXIR_PLUGIN_DIRS "" CACHE STRING "Semicolon-separated plugin directories compiled into the engine")
include(cmake/LuxirPlugins.cmake)
# Binary dirs carry the list index so two plugin dirs with one basename do not collide.
set(_plugin_index 0)
foreach(_plugin_dir IN LISTS LUXIR_PLUGIN_DIRS)
  cmake_path(ABSOLUTE_PATH _plugin_dir BASE_DIRECTORY "${CMAKE_SOURCE_DIR}" NORMALIZE)
  cmake_path(GET _plugin_dir FILENAME _plugin_base)
  math(EXPR _plugin_index "${_plugin_index} + 1")
  add_subdirectory("${_plugin_dir}" "${CMAKE_BINARY_DIR}/plugins/${_plugin_index}_${_plugin_base}")
endforeach()
add_subdirectory(test/plugins/example "${CMAKE_BINARY_DIR}/plugins/example")
luxir_finalize_plugins()

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

find_path(GTL_INCLUDE_DIRS "gtl/bit_vector.hpp")
target_include_directories(luxir_lib PRIVATE ${GTL_INCLUDE_DIRS})

# Link the whole archive so every executable using luxir_lib defines the malloc
# family and operator new/delete regardless of link order. The executable then
# exports them, and calls from shared libraries (libstdc++, glibc) bind there.
# Such executables cannot define their own global operator new/delete.
if(LUXIR_JEMALLOC)
  target_link_libraries(luxir_lib PUBLIC "$<LINK_LIBRARY:WHOLE_ARCHIVE,luxir_jemalloc>")
endif()
set_property(SOURCE src/luxir/util/ProcessAllocator.cpp APPEND PROPERTY
  COMPILE_DEFINITIONS
    "LUXIR_JEMALLOC=$<BOOL:${LUXIR_JEMALLOC}>"
    "LUXIR_SANITIZER=$<BOOL:${_luxir_sanitizer}>"
    "LUXIR_SANITIZER_NAME=\"${_luxir_sanitizer}\"")

target_link_libraries(luxir_lib PRIVATE TBB::tbb)
get_target_property(_luxir_tbb_type TBB::tbb TYPE)
if(_luxir_tbb_type STREQUAL "STATIC_LIBRARY")
  # Preserve TBB's internal allocator when its weak references cannot dlopen
  # tbbmalloc. This does not replace the application's malloc/free.
  target_link_libraries(luxir_lib PRIVATE TBB::tbbmalloc)
  # Mach-O symbol names carry a leading underscore.
  set(_luxir_sym "")
  if(APPLE)
    set(_luxir_sym _)
  endif()
  target_link_options(luxir_lib INTERFACE
    -Wl,-u,${_luxir_sym}scalable_malloc -Wl,-u,${_luxir_sym}scalable_free
    -Wl,-u,${_luxir_sym}scalable_aligned_malloc -Wl,-u,${_luxir_sym}scalable_aligned_free)
endif()
target_link_libraries(luxir_lib PRIVATE xxHash::xxhash)
target_link_libraries(luxir_lib PUBLIC Boost::core Boost::thread Boost::sort)
target_link_libraries(luxir_lib PRIVATE fastpfor)
# Concrete luxir::api wire model (post-cutover): hand-written concrete structs + out-of-line
# pb_meta/glz::meta (luxir_proto_concrete) carrying hpp_proto::runtime (is_utf8) + glaze.
# libprotobuf for google::protobuf::Arena (kept as the allocator) and the DescriptorPool used
# by descriptor-only reflection; grpc++ for the server. The trait-templated luxir_proto_hpp
# was dropped - no TU includes "protos/luxir*.pb.hpp" anymore. myproto (the protobuf message
# classes + typed stubs) is also intentionally gone.
target_link_libraries(luxir_lib PUBLIC luxir_proto_concrete)
target_link_libraries(luxir_lib PUBLIC protobuf::libprotobuf)
target_link_libraries(luxir_lib PUBLIC gRPC::grpc++ gRPC::grpc++_reflection)
# IANA zones decode from the system's compiled TZif files (DateTime.cpp).
target_link_libraries(luxir_lib PRIVATE absl::time)
# log.h is included by tests and other dependents; propagate spdlog's compiled
# mode so every TU shares the same default-logger registry.
target_link_libraries(luxir_lib PUBLIC spdlog::spdlog)
target_link_libraries(luxir_lib PRIVATE CLI11::CLI11)
target_link_libraries(luxir_lib PRIVATE lz4::lz4)
target_link_libraries(luxir_lib PRIVATE zstd::libzstd)
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
