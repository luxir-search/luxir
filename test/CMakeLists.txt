find_package(GTest CONFIG REQUIRED)
find_package(benchmark REQUIRED)

file(GLOB_RECURSE Test_SOURCES *.cpp *.h)

add_executable(solux_test ${Test_SOURCES})
target_include_directories(solux_test PRIVATE .)
target_link_libraries(solux_test PRIVATE solux_lib benchmark::benchmark GTest::gtest)
