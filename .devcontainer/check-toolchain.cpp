// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <chrono>
#include <cstdlib>
#include <format>
#include <iostream>
#include <stdexcept>
#include <string_view>

static_assert(__cplusplus >= 202400L);

int main(int argc, char** argv) {
  if (argc == 2) {
    int* values = new int[4]{};
    values[std::atoi(argv[1])] = 7;
    std::cout << values[0] << '\n';
    delete[] values;
    return 0;
  }
  try {
    throw std::runtime_error("unwind works");
  } catch (const std::runtime_error& e) {
    if (std::string_view(e.what()) != "unwind works") return 1;
  }
  int workers = 0;
  #pragma omp parallel reduction(+:workers)
  {
    workers++;
  }
  if (workers < 1) return 2;
  auto* zone = std::chrono::locate_zone("America/New_York");
  std::cout << std::format("GCC {}; timezone {}; OpenMP workers {}\n",
                           __VERSION__, zone->name(), workers);
}
