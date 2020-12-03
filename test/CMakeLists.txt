find_package(GTest CONFIG REQUIRED)    #TODO: should this be in the "test" directory?

file(GLOB_RECURSE Test_SOURCES "*.cpp")

add_executable(mytest ${Test_SOURCES})
target_link_libraries(mytest PUBLIC solux_lib)
#target_link_libraries(mytest PUBLIC solux_lib simdcomp_a)


# BUG with MSVC/vcpkg!  google mock main doesn't find any tests to run!
# https://github.com/google/googletest/issues/2157
#target_link_libraries(mytest PRIVATE GTest::gmock GTest::gtest GTest::gmock_main GTest::gtest_main)
target_link_libraries(mytest PRIVATE GTest::gtest GTest::gtest_main)

#
# target_link_libraries(runBasicTests solux_lib SIMDCompressionAndIntersection)

