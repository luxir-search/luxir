// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <exception>
#include <mutex>
#include <stdexcept>

#include "luxir/plugin/Plugin.h"

namespace luxir::detail {

// Once-only initialization with stable adder identity and terminal failure.
class PluginInit {
  std::once_flag once;
  void (*adder)(PluginRegistry&) = nullptr;
  std::exception_ptr failure;

  static void noPlugins(PluginRegistry&) {}

public:
  template <typename Initialize>
  void run(void (*addPlugins)(PluginRegistry&), Initialize&& initialize) {
    if (addPlugins == nullptr) addPlugins = noPlugins;
    std::call_once(once, [&] {
      adder = addPlugins;
      try {
        initialize(addPlugins);
      } catch (...) {
        // Complete once even on failure: registries may be partially populated
        // or frozen. No registry state may be relied on; the process must exit.
        failure = std::current_exception();
      }
    });
    if (failure) std::rethrow_exception(failure);
    if (adder != addPlugins) throw std::logic_error("initPlugins called with a different plugin adder");
  }
};

}  // namespace luxir::detail
