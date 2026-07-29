find_package(GTest CONFIG REQUIRED)
find_package(benchmark REQUIRED)

# CONFIGURE_DEPENDS: re-glob on build so a newly added test/helper .cpp is picked up
# without a manual reconfigure (otherwise new files link as undefined symbols).
file(GLOB_RECURSE Test_SOURCES CONFIGURE_DEPENDS *.cpp *.h)

add_executable(solux_test ${Test_SOURCES})
target_include_directories(solux_test PRIVATE .)
target_link_libraries(solux_test PRIVATE solux_lib benchmark::benchmark GTest::gtest)
# HttpApiTest's helper (test/HttpReq.h) parses NDJSON with glaze (via solux_lib) and
# drives the HTTP server with a header-only Boost.Beast client.
# uni-algo (vendored): AnalysisTest exercises Unicode case folding / NFKC / UAX#29
# segmentation directly to gate conformance before the real filters are built.
target_link_libraries(solux_test PRIVATE uni_algo)

# gtest/benchmark are the dominant parse cost across the ~40 test TUs; add them
# to the shared heavy-header set (SOLUX_PCH_COMMON comes from the root CMake).
target_precompile_headers(solux_test PRIVATE
  ${SOLUX_PCH_COMMON}
  <gtest/gtest.h>
  <benchmark/benchmark.h>
)

# Compile the same exhaustive decoded-successor tests with AVX-512 forcibly
# unavailable so the scalar portability path is exercised on AVX-512 hosts.
add_executable(decoded_successor_no_avx512_test
  basic_tests/DecodedSuccessorTest.cpp)
target_include_directories(decoded_successor_no_avx512_test PRIVATE .)
target_link_libraries(decoded_successor_no_avx512_test PRIVATE GTest::gtest_main)
target_compile_options(decoded_successor_no_avx512_test PRIVATE -mno-avx512f)
target_compile_definitions(decoded_successor_no_avx512_test PRIVATE
  SOLUX_EXPECT_NO_AVX512=1)


# (solux_smoke removed: it was migration scaffolding to run SmokeTest while solux_test was
# still on the protobuf API. solux_test now compiles SmokeTest.cpp on the concrete solux::api,
# so the standalone target is redundant.)


function(download fname url hash)
  if(NOT EXISTS ${fname})
    file(DOWNLOAD ${url} ${fname} TIMEOUT 20 EXPECTED_HASH SHA256=${hash} TLS_VERIFY ON)
  endif()
endfunction(download)

if(NOT DEFINED $ENV{TMP})
  set(ENV{TMP} /tmp)
endif()

set(SOLUX_TMP "$ENV{TMP}/solux")
file(MAKE_DIRECTORY ${SOLUX_TMP})
download("${SOLUX_TMP}/book.txt" http://www.gutenberg.org/files/2600/2600-0.txt e4bcf9042609b62c7de72a6f1b311f54c412943a9d641b7efcf79a464b5f31c8)

# Unicode 15.1.0 conformance files for AnalysisTest (must match uni-algo's
# pinned Unicode version: deps/uni-algo/include/uni_algo/impl/impl_unicode_version.h).
# If a download fails (offline), the conformance tests GTEST_SKIP rather than fail.
download("${SOLUX_TMP}/WordBreakTest-15.1.0.txt" https://www.unicode.org/Public/15.1.0/ucd/auxiliary/WordBreakTest.txt 69506b2253b8defb1f15b1476397376285cfda3bafd9304ee50d48bbfa0a2ab4)
download("${SOLUX_TMP}/CaseFolding-15.1.0.txt" https://www.unicode.org/Public/15.1.0/ucd/CaseFolding.txt 4e55acfdc32825a22e87670e9056a3bf94ad7c5400065778e9e10f8314372bcf)
