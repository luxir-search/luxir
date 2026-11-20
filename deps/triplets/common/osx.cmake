list(APPEND VCPKG_HASH_ADDITIONAL_FILES "${CMAKE_CURRENT_LIST_FILE}")
set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CMAKE_SYSTEM_NAME Darwin)
set(VCPKG_OSX_ARCHITECTURES arm64)
# Oldest macOS that binaries linked against these libraries can run on.
list(APPEND VCPKG_HASH_ADDITIONAL_FILES "${CMAKE_CURRENT_LIST_DIR}/macos-version.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/macos-version.cmake")
set(VCPKG_OSX_DEPLOYMENT_TARGET ${LUXIR_MACOS_DEPLOYMENT_TARGET})
# libstdc++ assumes the full C library, which g++ ensures on Linux by
# predefining _GNU_SOURCE. Keep it visible when a port selects a POSIX level.
set(VCPKG_CXX_FLAGS "-D_DARWIN_C_SOURCE")
# Avoid static oneTBB's unsupported IPO combination.
set(VCPKG_CMAKE_CONFIGURE_OPTIONS -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=OFF)
