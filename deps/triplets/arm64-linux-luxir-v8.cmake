include("${CMAKE_CURRENT_LIST_DIR}/common/linux.cmake")
set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_C_FLAGS "-march=armv8-a -mtune=generic")
set(VCPKG_CXX_FLAGS "-march=armv8-a -mtune=generic")
