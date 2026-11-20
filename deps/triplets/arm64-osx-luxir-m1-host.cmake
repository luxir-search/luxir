list(APPEND VCPKG_HASH_ADDITIONAL_FILES "${CMAKE_CURRENT_LIST_DIR}/arm64-osx-luxir-m1.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/arm64-osx-luxir-m1.cmake")
# Code generators need unsanitized executables, not a second set of Debug libraries.
set(VCPKG_BUILD_TYPE release)
