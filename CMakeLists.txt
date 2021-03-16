cmake_minimum_required(VERSION 3.16)
project(Solux VERSION 0.1)
set(CMAKE_CXX_STANDARD 20)
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
find_package (Boost COMPONENTS REQUIRED thread)

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
#set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++2a -g -O1 -fno-omit-frame-pointer -fsanitize=address")
#set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++2a -g -O0 -fno-omit-frame-pointer -fsanitize=address")     #don't optimize stuff out
#set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++2a -g -O0 -fno-omit-frame-pointer -fsanitize=address -DMEM_SCRIBBLE") #scribble over heap allocations before use
#set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++2a -g -O1 -fno-omit-frame-pointer -fsanitize=address -DMEMPOOL_MALLOC")  #use separate allocations
#set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++2a -g -O1 -fno-omit-frame-pointer -fsanitize=thread -fPIE -pie")
# NOTE: memory sanitizer needs *everything* including linked libc++ to be compiled with it.
# see https://github.com/google/sanitizers/wiki/MemorySanitizerLibcxxHowTo
#set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++2a -g -O1 -fno-omit-frame-pointer -fsanitize=memory -fsanitize-memory-track-origins -fPIE -pie -fno-optimize-sibling-calls")
#set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++2a -O3 -DNDEBUG -fstrict-aliasing")

message(STATUS "CMAKE_CXX_COMPILER_ID=${CMAKE_CXX_COMPILER_ID}")
if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -mavx -mssse3 -march=native -mtune=native")
  add_compile_options(
          "$<$<CONFIG:RELEASE>:-g>"
          "$<$<CONFIG:DEBUG>:-D_GLIBCXX_DEBUG>"
  )
  add_compile_options(-Wall -Wextra)
  # NOTE: simdcomp lib is compiled with -D_GLIBCXX_DEBUG for debugging lib... and if our files aren't, it causes a segv.
elseif (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -mavx -mssse3 -march=native -mtune=native")   # native just for dev... lock down more later?
  add_compile_options(
          "$<$<CONFIG:RELEASE>:-g>"
          "$<$<CONFIG:DEBUG>:-D_GLIBCXX_DEBUG>"
  )
  add_compile_options(-Wall -Wextra)
elseif (CMAKE_CXX_COMPILER_ID STREQUAL "Intel")
elseif (CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -D__SSSE3__ -arch:AVX")
  add_compile_options(/W4)
  #if debugging?
  #add_link_options("/nodefaultlib:msvcrt")
endif()



#
# grpc/protobuf, adapted from the helloworld example
#
set(protobuf_MODULE_COMPATIBLE TRUE)
find_package(Protobuf CONFIG REQUIRED)
message(STATUS "Using protobuf ${Protobuf_VERSION}")

set(_PROTOBUF_LIBPROTOBUF protobuf::libprotobuf)
set(_REFLECTION gRPC::grpc++_reflection)
if(CMAKE_CROSSCOMPILING)
  find_program(_PROTOBUF_PROTOC protoc)
else()
  set(_PROTOBUF_PROTOC $<TARGET_FILE:protobuf::protoc>)
endif()

# Find gRPC installation
# Looks for gRPCConfig.cmake file installed by gRPC's cmake installation.
find_package(gRPC CONFIG REQUIRED)
message(STATUS "Using gRPC ${gRPC_VERSION}")

set(_GRPC_GRPCPP gRPC::grpc++)
if(CMAKE_CROSSCOMPILING)
  find_program(_GRPC_CPP_PLUGIN_EXECUTABLE grpc_cpp_plugin)
else()
  set(_GRPC_CPP_PLUGIN_EXECUTABLE $<TARGET_FILE:gRPC::grpc_cpp_plugin>)
endif()

# Proto file
get_filename_component(solux_proto "./protos/solux.proto" ABSOLUTE)
get_filename_component(solux_types_proto "./protos/solux_types.proto" ABSOLUTE)
get_filename_component(solux_proto_path "${solux_proto}" PATH)

# Generated sources
set(solux_proto_srcs "${CMAKE_CURRENT_BINARY_DIR}/solux.pb.cc" "${CMAKE_CURRENT_BINARY_DIR}/solux_types.pb.cc")
set(solux_proto_hdrs "${CMAKE_CURRENT_BINARY_DIR}/solux.pb.h" "${CMAKE_CURRENT_BINARY_DIR}/solux_types.pb.h")
set(solux_grpc_srcs "${CMAKE_CURRENT_BINARY_DIR}/solux.grpc.pb.cc")
set(solux_grpc_hdrs "${CMAKE_CURRENT_BINARY_DIR}/solux.grpc.pb.h")
add_custom_command(
        OUTPUT "${solux_proto_srcs}" "${solux_proto_hdrs}" "${solux_grpc_srcs}" "${solux_grpc_hdrs}"
        COMMAND ${_PROTOBUF_PROTOC}
        ARGS --grpc_out "${CMAKE_CURRENT_BINARY_DIR}" --experimental_allow_proto3_optional
        --cpp_out "${CMAKE_CURRENT_BINARY_DIR}"
        -I "${solux_proto_path}"
        --plugin=protoc-gen-grpc="${_GRPC_CPP_PLUGIN_EXECUTABLE}"
        "${solux_proto}" "${solux_types_proto}"
        DEPENDS "${solux_proto}")

# Include generated *.pb.h files
include_directories("${CMAKE_CURRENT_BINARY_DIR}")
########### end grpc/protobuf



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

find_path(PARALLEL_HASHMAP_INCLUDE_DIRS "parallel_hashmap/btree.h")
target_include_directories(solux_lib PRIVATE ${PARALLEL_HASHMAP_INCLUDE_DIRS})

target_link_libraries(solux_lib PRIVATE xxHash::xxhash)
target_link_libraries(solux_lib PRIVATE ${Boost_LIBRARIES})
target_link_libraries(solux_lib PRIVATE debug simdcomp_ad)
target_link_libraries(solux_lib PRIVATE optimized simdcomp_a)
target_link_libraries(solux_lib PRIVATE myproto)