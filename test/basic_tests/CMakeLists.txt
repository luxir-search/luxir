include_directories(${gtest_SOURCE_DIR}/include ${gtest_SOURCE_DIR})

add_executable(runBasicTests
        BasicTest.cpp
        TermValHashTest.cpp
        StreamTest.cpp
        PostingsWriterTest.cpp
        SIMDCompTest.cpp
        SoluxTest.h)

target_link_libraries(runBasicTests gtest gtest_main)
target_link_libraries(runBasicTests solux_lib SIMDCompressionAndIntersection)

