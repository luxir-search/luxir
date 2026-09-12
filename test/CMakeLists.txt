# Copyright 2020-2026 Yonik Seeley and Luxir contributors
# SPDX-License-Identifier: Apache-2.0

find_package(GTest CONFIG REQUIRED)
find_package(benchmark REQUIRED)

# CONFIGURE_DEPENDS: re-glob on build so a newly added test/helper .cpp is picked up
# without a manual reconfigure (otherwise new files link as undefined symbols).
file(GLOB_RECURSE Test_SOURCES CONFIGURE_DEPENDS *.cpp *.h)

add_executable(luxir_test ${Test_SOURCES})
target_include_directories(luxir_test PRIVATE .)
target_link_libraries(luxir_test PRIVATE luxir_lib benchmark::benchmark GTest::gtest)
# HttpApiTest's helper (test/HttpReq.h) parses NDJSON with glaze (via luxir_lib) and
# drives the HTTP server with a header-only Boost.Beast client.
# uni-algo (vendored): AnalysisTest exercises Unicode case folding / NFKC / UAX#29
# segmentation directly to gate conformance before the real filters are built.
target_link_libraries(luxir_test PRIVATE uni_algo)

# gtest/benchmark are the dominant parse cost across the ~40 test TUs; add them
# to the shared heavy-header set (LUXIR_PCH_COMMON comes from the root CMake).
target_precompile_headers(luxir_test PRIVATE
  ${LUXIR_PCH_COMMON}
  <gtest/gtest.h>
  <benchmark/benchmark.h>
)

# Compile the same exhaustive decoded-successor tests with AVX-512 forcibly
# unavailable so AVX2 hosts exercise the AVX2 path.
add_executable(decoded_successor_no_avx512_test
  basic_tests/DecodedSuccessorTest.cpp)
target_include_directories(decoded_successor_no_avx512_test PRIVATE .)
target_link_libraries(decoded_successor_no_avx512_test PRIVATE GTest::gtest_main)
target_compile_options(decoded_successor_no_avx512_test PRIVATE -mno-avx512f)
target_compile_definitions(decoded_successor_no_avx512_test PRIVATE
  LUXIR_EXPECT_NO_AVX512=1)

# Compile the same tests without AVX-512 or AVX2 so the scalar portability path
# is exercised on SIMD hosts.
add_executable(decoded_successor_scalar_test
  basic_tests/DecodedSuccessorTest.cpp)
target_include_directories(decoded_successor_scalar_test PRIVATE .)
target_link_libraries(decoded_successor_scalar_test PRIVATE GTest::gtest_main)
target_compile_options(decoded_successor_scalar_test PRIVATE
  -mno-avx512f -mno-avx2)
target_compile_definitions(decoded_successor_scalar_test PRIVATE
  LUXIR_EXPECT_SCALAR=1)


# (luxir_smoke removed: it was migration scaffolding to run SmokeTest while luxir_test was
# still on the protobuf API. luxir_test now compiles SmokeTest.cpp on the concrete luxir::api,
# so the standalone target is redundant.)


function(download fname url hash)
  if(EXISTS "${fname}")
    file(SHA256 "${fname}" actual_hash)
    if(actual_hash STREQUAL hash)
      return()
    endif()
    file(REMOVE "${fname}")
  endif()
  file(DOWNLOAD "${url}" "${fname}.part" TIMEOUT 20 TLS_VERIFY ON STATUS result)
  list(GET result 0 status)
  if(status EQUAL 0)
    file(SHA256 "${fname}.part" actual_hash)
    if(actual_hash STREQUAL hash)
      file(RENAME "${fname}.part" "${fname}")
      return()
    endif()
    set(result "SHA256 mismatch: expected ${hash}, received ${actual_hash}")
  endif()
  file(REMOVE "${fname}.part")
  message(WARNING "Test data unavailable: ${url}: ${result}")
endfunction(download)

# Keep downloaded fixtures with the build, including across disposable containers.
set(LUXIR_TEST_DATA_DIR "${CMAKE_CURRENT_BINARY_DIR}/data")
file(MAKE_DIRECTORY "${LUXIR_TEST_DATA_DIR}")
set_property(SOURCE test/TestData.cpp APPEND PROPERTY COMPILE_DEFINITIONS
  "LUXIR_TEST_DATA_DIR=\"${LUXIR_TEST_DATA_DIR}\"")
download("${LUXIR_TEST_DATA_DIR}/book.txt" https://www.gutenberg.org/files/2600/2600-0.txt e4bcf9042609b62c7de72a6f1b311f54c412943a9d641b7efcf79a464b5f31c8)

# Unicode 15.1.0 conformance files for AnalysisTest (must match uni-algo's
# pinned Unicode version: deps/uni-algo/include/uni_algo/impl/impl_unicode_version.h).
# If a download fails (offline), the conformance tests GTEST_SKIP rather than fail.
download("${LUXIR_TEST_DATA_DIR}/WordBreakTest-15.1.0.txt" https://www.unicode.org/Public/15.1.0/ucd/auxiliary/WordBreakTest.txt 69506b2253b8defb1f15b1476397376285cfda3bafd9304ee50d48bbfa0a2ab4)
download("${LUXIR_TEST_DATA_DIR}/CaseFolding-15.1.0.txt" https://www.unicode.org/Public/15.1.0/ucd/CaseFolding.txt 4e55acfdc32825a22e87670e9056a3bf94ad7c5400065778e9e10f8314372bcf)
