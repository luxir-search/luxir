// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Inverter.h"

namespace luxir {

class ObjectFields {
  // Handlers cached by raw field position: a request's docs usually share a shape,
  // so a hit costs one name compare.
  std::vector<Inverter::InputHandler*> handlers;
  // The current doc's fields after last-wins dedup, in first-seen order.
  std::vector<std::pair<Inverter::InputHandler*, const IndexVal*>> fields;

public:
  // The resolver returns an optional logical name; nullopt drops the field.
  // Keep the concrete resolver visible when compiling the object loop.
  template <typename Names>
  LUXIR_INLINE void index(Inverter& inverter, const api::Map& doc, const Names& names) {
    size_t nFields = doc.fields.size();
    if (handlers.size() < nFields) {
      handlers.resize(nFields);
    }

    // Duplicate keys post-mapping collapse last-wins (protobuf map semantics). The
    // handler is a name's identity, so its claim finds an earlier occurrence without
    // a per-doc map; a repeat replaces the value at the first occurrence's position.
    fields.clear();
    size_t raw = 0;
    for (const auto& [name, val] : doc.fields) {
      Inverter::InputHandler*& handler = handlers[raw++];
      auto fname = names.resolve(name);
      if (!fname) continue;
      // A cached handler proves its name was accepted; creation validates a
      // new name (a derived selector is not a document key, and the engine
      // assigns _version_).
      if (handler == nullptr || *handler != *fname) {
        if (*fname == "_version_") {
          throw DocumentError("Field '_version_' is assigned by the engine; optimistic concurrency is not supported");
        }
        handler = &inverter.getIndexHandler(*fname);
      }
      uint32_t slot = handler->claim(inverter, (uint32_t)fields.size());
      if (slot == fields.size()) fields.emplace_back(handler, &*val);
      else fields[slot].second = &*val;
    }
    for (const auto& [handler, val] : fields) {
      handler->index(inverter, *val);
    }
  }
};

} // namespace luxir
