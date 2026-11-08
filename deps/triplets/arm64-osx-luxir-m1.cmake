include("${CMAKE_CURRENT_LIST_DIR}/common/osx.cmake")
# A fixed baseline rather than the host CPU: later Apple cores add only BF16
# and int8 matmul, which these libraries do not use, so one install serves
# local builds and binaries for any Apple silicon Mac.
set(VCPKG_C_FLAGS "-mcpu=apple-m1")
string(APPEND VCPKG_CXX_FLAGS " -mcpu=apple-m1")
