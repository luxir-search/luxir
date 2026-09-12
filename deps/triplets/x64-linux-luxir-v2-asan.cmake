include("${CMAKE_CURRENT_LIST_DIR}/x64-linux-luxir-v2.cmake")
string(APPEND VCPKG_C_FLAGS " -g -fsanitize=address -fno-omit-frame-pointer")
string(APPEND VCPKG_CXX_FLAGS " -g -fsanitize=address -fno-omit-frame-pointer")
set(VCPKG_LINKER_FLAGS "-fsanitize=address")
