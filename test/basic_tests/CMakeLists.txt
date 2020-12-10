include_directories(${gtest_SOURCE_DIR}/include ${gtest_SOURCE_DIR})

add_executable(runBasicTests
        basic_test.cpp
        StrValHash_test.cpp
        Stream_test.cpp
        PostingsWriter_test.cpp
        simd_test.cpp
        test_util.h )

target_link_libraries(runBasicTests gtest gtest_main)
target_link_libraries(runBasicTests solux_lib SIMDCompressionAndIntersection)

