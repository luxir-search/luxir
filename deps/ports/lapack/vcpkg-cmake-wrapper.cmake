if(APPLE)
  # Accelerate supplies both BLAS and LAPACK, as the blas port selects there.
  set(BLA_VENDOR Apple)
  _find_package(${ARGS})
else()
  # OpenBLAS contains both BLAS and the translated C LAPACK routines.
  # FindLAPACK's OpenBLAS branch checks cheev_ using the BLAS library itself.
  _find_package(BLAS REQUIRED)
  set(BLA_VENDOR OpenBLAS)
  set(BLA_STATIC ON)
  _find_package(${ARGS})
  unset(BLA_STATIC)
endif()
unset(BLA_VENDOR)
