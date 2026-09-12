// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>

// FAISS uses these ten entry points, all with 32-bit Fortran INTEGER ABI.
extern "C" {
void dsyev_(const char*, const char*, const int*, double*, const int*, double*, double*, const int*, int*);
void dgesvd_(const char*, const char*, const int*, const int*, double*, const int*, double*, double*, const int*, double*, const int*, double*, const int*, int*);
void sgesvd_(const char*, const char*, const int*, const int*, float*, const int*, float*, float*, const int*, float*, const int*, float*, const int*, int*);
void dgetrf_(const int*, const int*, double*, const int*, int*, int*);
void sgetrf_(const int*, const int*, float*, const int*, int*, int*);
void dgetri_(const int*, double*, const int*, const int*, double*, const int*, int*);
void sgetri_(const int*, float*, const int*, const int*, float*, const int*, int*);
void sgeqrf_(const int*, const int*, float*, const int*, float*, float*, const int*, int*);
void sorgqr_(const int*, const int*, const int*, float*, const int*, const float*, float*, const int*, int*);
void sgelsd_(const int*, const int*, const int*, float*, const int*, float*, const int*, float*, const float*, int*, float*, const int*, int*, int*);
const char* openblas_get_config();
const char* openblas_get_corename();
}

void check(bool ok) {
  if (!ok) {
    std::cerr << "C LAPACK numerical/ABI check failed\n";
    std::exit(1);
  }
}

int main() {
  const int n = 2, lwork = 1024;
  int info = -1;
  double work[1024], a[] = {2, 1, 1, 2}, eigenvalues[2];
  dsyev_("V", "U", &n, a, &n, eigenvalues, work, &lwork, &info);
  check(info == 0 && std::abs(eigenvalues[0] - 1) < 1e-12 && std::abs(eigenvalues[1] - 3) < 1e-12);
  for (int j = 0; j < n; j++) {
    check(std::abs(2*a[2*j] + a[2*j+1] - eigenvalues[j]*a[2*j]) < 1e-12);
    check(std::abs(a[2*j] + 2*a[2*j+1] - eigenvalues[j]*a[2*j+1]) < 1e-12);
  }

  // Reconstruct a non-symmetric matrix from both precisions' full SVD.
  double input[] = {1, 3, 2, 5}, d[4], singular[2], u[4], vt[4];
  std::copy_n(input, 4, d);
  dgesvd_("A", "A", &n, &n, d, &n, singular, u, &n, vt, &n, work, &lwork, &info);
  check(info == 0);
  float f[4], fs[2], fu[4], fvt[4], fw[1024];
  std::copy_n(input, 4, f);
  sgesvd_("A", "A", &n, &n, f, &n, fs, fu, &n, fvt, &n, fw, &lwork, &info);
  check(info == 0);
  for (int col = 0; col < n; col++) for (int row = 0; row < n; row++) {
    double rebuilt = 0, frebuilt = 0;
    for (int k = 0; k < n; k++) {
      rebuilt += u[row+2*k] * singular[k] * vt[k+2*col];
      frebuilt += fu[row+2*k] * fs[k] * fvt[k+2*col];
    }
    check(std::abs(rebuilt - input[row+2*col]) < 1e-12);
    check(std::abs(frebuilt - input[row+2*col]) < 1e-5);
  }

  // LU and inverse, including a pivot, checked against A * inverse(A) = I.
  int pivots[2];
  std::copy_n(input, 4, d);
  dgetrf_(&n, &n, d, &n, pivots, &info); check(info == 0);
  dgetri_(&n, d, &n, pivots, work, &lwork, &info); check(info == 0);
  std::copy_n(input, 4, f);
  sgetrf_(&n, &n, f, &n, pivots, &info); check(info == 0);
  sgetri_(&n, f, &n, pivots, fw, &lwork, &info); check(info == 0);
  for (int col = 0; col < n; col++) for (int row = 0; row < n; row++) {
    double product = 0, fproduct = 0;
    for (int k = 0; k < n; k++) {
      product += input[row+2*k] * d[k+2*col];
      fproduct += input[row+2*k] * f[k+2*col];
    }
    check(std::abs(product - (row == col)) < 1e-12);
    check(std::abs(fproduct - (row == col)) < 1e-5);
  }

  // QR: generate Q, then reconstruct the rectangular input from Q * R.
  const int m = 3, nrhs = 1;
  float rect[] = {1, 0, 1, 0, 1, 1}, qr[6], tau[2];
  std::copy_n(rect, 6, qr);
  sgeqrf_(&m, &n, qr, &m, tau, fw, &lwork, &info); check(info == 0);
  float r[] = {qr[0], 0, qr[3], qr[4]};
  sorgqr_(&m, &n, &n, qr, &m, tau, fw, &lwork, &info); check(info == 0);
  for (int col = 0; col < n; col++) for (int row = 0; row < m; row++) {
    float rebuilt = 0;
    for (int k = 0; k < n; k++) rebuilt += qr[row+3*k] * r[k+2*col];
    check(std::abs(rebuilt - rect[row+3*col]) < 1e-5);
  }

  float rhs[] = {2, 3, 5}, rcond = -1;
  int rank = 0, iwork[1024];
  sgelsd_(&m, &n, &nrhs, rect, &m, rhs, &m, fs, &rcond, &rank, fw, &lwork, iwork, &info);
  check(info == 0 && rank == 2 && std::abs(rhs[0] - 2) < 1e-5 && std::abs(rhs[1] - 3) < 1e-5);
  std::cout << "All ten LAPACK entry points passed: " << openblas_get_config()
            << "; selected " << openblas_get_corename() << '\n';
}
