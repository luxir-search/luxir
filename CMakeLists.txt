cmake_minimum_required(VERSION 3.16)
project(Solux VERSION 0.1)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED True)

# This keeps dlls and executables together, which makes windows happy.
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

include_directories(SYSTEM ./src)

find_package(xxHash CONFIG REQUIRED)

#from clion docs
find_package(Boost)
IF (Boost_FOUND)
    include_directories(${Boost_INCLUDE_DIR})
endif()
set (Boost_USE_STATIC_LIBS OFF) # enable dynamic linking
set (Boost_USE_MULTITHREAD ON)  # enable multithreading
find_package (Boost COMPONENTS REQUIRED core thread sort)

find_package(TBB CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(CLI11 CONFIG REQUIRED)
find_package(lz4 CONFIG REQUIRED)
# FAISS for vector ANN aux indexes (gcc vcpkg presets only).
# FAISS exports OpenMP::OpenMP_CXX in its imported-target interface, so we must
# locate OpenMP before find_package(faiss).
find_package(OpenMP REQUIRED)
find_package(faiss CONFIG REQUIRED)

include_directories(deps)
link_directories(deps/simdcomp)

#Print out all env vars... useful while build system is in development
get_cmake_property(_variableNames VARIABLES)
list (SORT _variableNames)
foreach (_variableName ${_variableNames})
    message(STATUS "${_variableName}=${${_variableName}}")
endforeach()

# set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-omit-frame-pointer")   #for profiling release
# sanitizer settings.. can leave this first one on while developing for linux
#set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++2b -g -O1 -fno-omit-frame-pointer -fsanitize=address")
#set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++2b -g -O0 -fno-omit-frame-pointer -fsanitize=address")     #don't optimize stuff out
#set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++2b -g -O0 -fno-omit-frame-pointer -fsanitize=address -DMEM_SCRIBBLE") #scribble over heap allocations before use
#set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++2b -g -O1 -fno-omit-frame-pointer -fsanitize=address -DMEMPOOL_MALLOC")  #use separate allocations
#set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++2b -g -O0 -fno-omit-frame-pointer -DMEMPOOL_MALLOC")  #use separate allocations, for valgrind
#set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++2b -g -O1 -fno-omit-frame-pointer -fsanitize=thread -fPIE -pie")
# NOTE: memory sanitizer needs *everything* including linked libc++ to be compiled with it.
# see https://github.com/google/sanitizers/wiki/MemorySanitizerLibcxxHowTo
#set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++2b -g -O1 -fno-omit-frame-pointer -fsanitize=memory -fsanitize-memory-track-origins -fPIE -pie -fno-optimize-sibling-calls")
#set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++2b -O3 -DNDEBUG -fstrict-aliasing")

message(STATUS "CMAKE_CXX_COMPILER_ID=${CMAKE_CXX_COMPILER_ID}")
if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -mavx -mssse3 -march=native -mtune=native")
  add_compile_options(
          "$<$<CONFIG:RELEASE>:-g>"
#          "$<$<CONFIG:DEBUG>:-D_GLIBCXX_DEBUG>"
  )
  # -Wno-unused-parameter: many virtual overrides must keep base-class signature; renaming params would just be noise.
  add_compile_options(-Wall -Wextra -Wno-unused-parameter)
  # NOTE: simdcomp lib is compiled with -D_GLIBCXX_DEBUG for debugging lib by default... and if our files aren't, it causes a segv
  # so make sure they match.  For now, we remove that flag from simdcomp.
elseif (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -mavx -mssse3 -march=native -mtune=native")   # native just for dev... lock down more later?
  add_compile_options(
          "$<$<CONFIG:RELEASE>:-g>"
#          "$<$<CONFIG:DEBUG>:-D_GLIBCXX_DEBUG>"
  )
  add_compile_options(-Wall -Wextra -Wno-unused-parameter)
elseif (CMAKE_CXX_COMPILER_ID STREQUAL "Intel")
elseif (CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -D__SSSE3__ -arch:AVX")
  add_compile_options(/W4)
  #if debugging?
  #add_link_options("/nodefaultlib:msvcrt")
endif()

# Include protobuf/grpc generated *.pb.h files
include_directories("${CMAKE_CURRENT_BINARY_DIR}")

file(GLOB_RECURSE Proj_SOURCES src/*.cpp src/*.h)
list(FILTER Proj_SOURCES EXCLUDE REGEX .*/main.cpp)

add_library(solux_lib STATIC ${Proj_SOURCES})

add_executable(solux src/solux/main.cpp)
target_link_libraries(solux solux_lib)

add_subdirectory(protos)
add_subdirectory(test)

if(ROBIN_HOOD_HASHING)
  find_path(ROBIN_HOOD_HASHING_INCLUDE_DIRS "robin_hood.h")
  target_include_directories(solux_lib PRIVATE ${ROBIN_HOOD_HASHING_INCLUDE_DIRS})
endif()

find_path(GTL_INCLUDE_DIRS "gtl/bit_vector.hpp")
target_include_directories(solux_lib PRIVATE ${GTL_INCLUDE_DIRS})

target_link_libraries(solux_lib PRIVATE TBB::tbb)
target_link_libraries(solux_lib PRIVATE xxHash::xxhash)
target_link_libraries(solux_lib PRIVATE ${Boost_LIBRARIES})
target_link_libraries(solux_lib PRIVATE debug simdcomp_ad)
target_link_libraries(solux_lib PRIVATE optimized simdcomp_a)
target_link_libraries(solux_lib PRIVATE myproto)
target_link_libraries(solux_lib PRIVATE spdlog::spdlog spdlog::spdlog_header_only)
target_link_libraries(solux_lib PRIVATE CLI11::CLI11)
target_link_libraries(solux_lib PRIVATE lz4::lz4)
target_link_libraries(solux_lib PRIVATE faiss)
