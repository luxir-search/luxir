include("${CMAKE_CURRENT_LIST_DIR}/common/linux.cmake")
if("$ENV{LUXIR_NATIVE_CPU}" STREQUAL "")
  message(FATAL_ERROR "Use deps/make_deps.sh to fingerprint the native CPU before building dependencies")
endif()
# Compiler identity alone cannot distinguish -march=native on different CPUs.
list(APPEND VCPKG_ENV_PASSTHROUGH LUXIR_NATIVE_CPU)
set(VCPKG_C_FLAGS "-march=native -mtune=native")
set(VCPKG_CXX_FLAGS "-march=native -mtune=native")
