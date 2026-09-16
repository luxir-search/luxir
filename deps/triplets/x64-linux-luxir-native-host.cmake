list(APPEND VCPKG_HASH_ADDITIONAL_FILES "${CMAKE_CURRENT_LIST_DIR}/x64-linux-luxir-native.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/x64-linux-luxir-native.cmake")
# Code generators need unsanitized executables, not a second set of Debug libraries.
set(VCPKG_BUILD_TYPE release)
