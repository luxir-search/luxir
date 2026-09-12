set(VCPKG_POLICY_EMPTY_PACKAGE enabled)
configure_file("${CURRENT_PORT_DIR}/vcpkg-cmake-wrapper.cmake"
               "${CURRENT_PACKAGES_DIR}/share/lapack/vcpkg-cmake-wrapper.cmake" COPYONLY)
