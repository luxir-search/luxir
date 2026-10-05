# Copyright 2020-2026 Yonik Seeley and Luxir contributors
# SPDX-License-Identifier: Apache-2.0

include_guard(GLOBAL)

# Discover installed libraries without selecting a compiler or installing packages.
if(APPLE AND NOT DEFINED VCPKG_TARGET_TRIPLET)
  find_program(LUXIR_BREW_EXECUTABLE brew)
  if(LUXIR_BREW_EXECUTABLE)
    execute_process(COMMAND "${LUXIR_BREW_EXECUTABLE}" --prefix
      OUTPUT_VARIABLE _luxir_brew_prefix OUTPUT_STRIP_TRAILING_WHITESPACE
      RESULT_VARIABLE _luxir_brew_result)
    if(_luxir_brew_result EQUAL 0)
      list(APPEND CMAKE_PREFIX_PATH "${_luxir_brew_prefix}")
      foreach(_formula xxhash boost tbb spdlog cli11 lz4 faiss protobuf grpc
                       glaze gtl googletest google-benchmark howard-hinnant-date
                       libomp openssl@3)
        list(APPEND CMAKE_PREFIX_PATH "${_luxir_brew_prefix}/opt/${_formula}")
      endforeach()
    endif()
  endif()
endif()

set(_luxir_missing_packages)
macro(luxir_require_config package formula)
  find_package(${package} CONFIG QUIET ${ARGN})
  if(NOT ${package}_FOUND)
    list(APPEND _luxir_missing_packages "${formula}")
  endif()
endmacro()

# Some system packages provide only headers and libraries, without CMake configs.
function(luxir_import_library target key header library)
  find_path(LUXIR_${key}_INCLUDE_DIR "${header}")
  find_library(LUXIR_${key}_LIBRARY NAMES "${library}")
  if(LUXIR_${key}_INCLUDE_DIR AND LUXIR_${key}_LIBRARY)
    add_library(${target} UNKNOWN IMPORTED GLOBAL)
    set_target_properties(${target} PROPERTIES
      IMPORTED_LOCATION "${LUXIR_${key}_LIBRARY}"
      INTERFACE_INCLUDE_DIRECTORIES "${LUXIR_${key}_INCLUDE_DIR}")
  endif()
endfunction()

find_package(xxHash CONFIG QUIET)
if(NOT TARGET xxHash::xxhash)
  luxir_import_library(xxHash::xxhash XXHASH xxhash.h xxhash)
  if(NOT TARGET xxHash::xxhash)
    list(APPEND _luxir_missing_packages xxhash)
  endif()
endif()

luxir_require_config(Boost boost COMPONENTS core thread sort)
luxir_require_config(TBB tbb)
luxir_require_config(spdlog spdlog)
luxir_require_config(CLI11 cli11)

find_package(lz4 CONFIG QUIET)
if(NOT TARGET lz4::lz4)
  luxir_import_library(lz4::lz4 LZ4 lz4.h lz4)
  if(NOT TARGET lz4::lz4)
    list(APPEND _luxir_missing_packages lz4)
  endif()
endif()

# AppleClang uses the separately installed OpenMP runtime.
if(CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
  find_path(LUXIR_OMP_INCLUDE_DIR omp.h
    HINTS "${_luxir_brew_prefix}/opt/libomp/include")
  find_library(LUXIR_OMP_LIBRARY NAMES omp
    HINTS "${_luxir_brew_prefix}/opt/libomp/lib")
  if(LUXIR_OMP_INCLUDE_DIR AND LUXIR_OMP_LIBRARY)
    foreach(_language C CXX)
      set(OpenMP_${_language}_FLAGS "-Xpreprocessor -fopenmp")
      set(OpenMP_${_language}_INCLUDE_DIR "${LUXIR_OMP_INCLUDE_DIR}")
      set(OpenMP_${_language}_LIB_NAMES omp)
    endforeach()
    set(OpenMP_omp_LIBRARY "${LUXIR_OMP_LIBRARY}")
  endif()
endif()
find_package(OpenMP QUIET)
if(NOT OpenMP_FOUND)
  if(APPLE)
    list(APPEND _luxir_missing_packages libomp)
  else()
    list(APPEND _luxir_missing_packages "OpenMP (matching compiler runtime and headers)")
  endif()
endif()

# FAISS's imported target refers to OpenMP::OpenMP_CXX.
luxir_require_config(faiss faiss)
find_package(Protobuf CONFIG QUIET)
if(NOT Protobuf_FOUND)
  find_package(Protobuf QUIET)
endif()
if(NOT Protobuf_FOUND)
  list(APPEND _luxir_missing_packages protobuf)
endif()
luxir_require_config(gRPC grpc)
luxir_require_config(glaze glaze)
find_path(GTL_INCLUDE_DIRS gtl/bit_vector.hpp)
if(NOT GTL_INCLUDE_DIRS)
  list(APPEND _luxir_missing_packages gtl)
endif()
find_package(GTest QUIET)
if(NOT GTest_FOUND)
  list(APPEND _luxir_missing_packages googletest)
endif()
luxir_require_config(benchmark google-benchmark)
if(APPLE)
  luxir_require_config(date howard-hinnant-date)
endif()

if(_luxir_missing_packages)
  list(REMOVE_DUPLICATES _luxir_missing_packages)
  string(JOIN ", " _luxir_missing_text ${_luxir_missing_packages})
  if(APPLE)
    string(JOIN " " _luxir_install_names ${_luxir_missing_packages})
    set(_luxir_install_help "Install them with: brew install ${_luxir_install_names}")
  else()
    set(_luxir_install_help
      "Install the corresponding development packages with your package manager, "
      "or provide their installation prefix through CMAKE_PREFIX_PATH.")
  endif()
  message(FATAL_ERROR
    "Missing Luxir dependencies: ${_luxir_missing_text}\n${_luxir_install_help}\n"
    "Then rerun CMake. See docs/dev/build-setup.md.")
endif()
