find_package(GTest CONFIG REQUIRED)
find_package(benchmark REQUIRED)

file(GLOB_RECURSE Test_SOURCES *.cpp *.h)

add_executable(solux_test ${Test_SOURCES})
target_include_directories(solux_test PRIVATE .)
target_link_libraries(solux_test PRIVATE solux_lib benchmark::benchmark GTest::gtest)

# gtest/benchmark are the dominant parse cost across the ~40 test TUs; add them
# to the shared heavy-header set (SOLUX_PCH_COMMON comes from the root CMake).
target_precompile_headers(solux_test PRIVATE
  ${SOLUX_PCH_COMMON}
  <gtest/gtest.h>
  <benchmark/benchmark.h>
)


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
