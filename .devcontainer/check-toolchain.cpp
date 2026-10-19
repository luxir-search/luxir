// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <chrono>
#include <cstdlib>
#include <format>
#include <iostream>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <thread>

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
  std::mutex mutex;
  std::condition_variable ready;
  bool done = false;
  std::jthread thread([&] {
    std::lock_guard lock(mutex);
    done = true;
    ready.notify_one();
  });
  std::unique_lock lock(mutex);
  ready.wait(lock, [&] { return done; });
  if (!std::filesystem::is_directory(std::filesystem::current_path())) return 3;
  auto* zone = std::chrono::locate_zone("America/New_York");
  std::cout << std::format("GCC {}; timezone {}; OpenMP workers {}\n",
                           __VERSION__, zone->name(), workers);
}
