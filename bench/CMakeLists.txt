find_package(benchmark REQUIRED)

file(GLOB_RECURSE Bench_SOURCES *.cpp *.h)

add_executable(solux_bench ${Bench_SOURCES})
target_link_libraries(solux_bench PUBLIC solux_lib benchmark::benchmark benchmark::benchmark_main)

