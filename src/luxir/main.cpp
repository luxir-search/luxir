// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "luxir_main.h"
#include "luxir/util/ErrorLog.h"

int main(int argc, char** argv) {
  try { return luxir_main(argc, argv); }
  catch (...) {
    luxir::logCurrentException({"process", {}, {}, {}});
    return 1;
  }
}
