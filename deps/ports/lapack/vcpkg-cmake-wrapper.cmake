# OpenBLAS contains both BLAS and the translated C LAPACK routines.
# FindLAPACK's OpenBLAS branch checks cheev_ using the BLAS library itself.
_find_package(BLAS REQUIRED)
set(BLA_VENDOR OpenBLAS)
set(BLA_STATIC ON)
_find_package(${ARGS})
unset(BLA_VENDOR)
unset(BLA_STATIC)
