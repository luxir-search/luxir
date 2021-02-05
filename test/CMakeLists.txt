find_package(GTest CONFIG REQUIRED)
find_package(benchmark REQUIRED)

file(GLOB_RECURSE Test_SOURCES *.cpp *.h)

add_executable(solux_test ${Test_SOURCES})
target_include_directories(solux_test PRIVATE .)
target_link_libraries(solux_test PRIVATE solux_lib benchmark::benchmark GTest::gtest)


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
download("${SOLUX_TMP}/book.txt" http://www.gutenberg.org/files/2600/2600-0.txt 18431bda910e27c8bf15df63a67b15bbe3d1acf104b402e81af9a464ed2a8622)
