// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "NestedInversion.h"
#include "luxir/schema/ValCoerce.h"

namespace luxir {

std::optional<std::string_view> NestedPath::Names::resolve(std::string_view name) const {
  auto found = path.names.find(name);
  if (found != path.names.end()) return std::string_view(found->second);
  auto qualified = Schema::qualify(std::string_view(path.name), name);
  if (!Schema::validFieldName(name) || name.find('.') != std::string_view::npos) {
    throw DocumentError("Invalid nested object key: " + qualified, "invalid_field_name");
  }
  PackedTerm local(inverter.pool, name);
  PackedTerm physical(inverter.pool, qualified);
  auto [entry, inserted] = path.names.emplace(std::string_view(local), physical);
  return std::string_view(entry->second);
}

NestedPath& NestedState::path(std::string_view name) {
  auto found = paths.find(name);
  if (found != paths.end()) return *found->second;
  auto path = inverter.pool.make_unique<NestedPath>(inverter, name, active);
  auto key = std::string_view(path->name);
  auto [entry, inserted] = paths.emplace(key, std::move(path));
  return *entry->second;
}

void NestedState::index(NestedPath& path, const IndexVal& val) {
  if (coerce::isNull(val)) return;
  bool array = coerce::isArray(val);
  if (!array && !std::holds_alternative<api::Map>(val.kind)) {
    throw DocumentError("Nested path '" + std::string(std::string_view(path.name)) + "' requires an object or array of objects");
  }
  // Reserve before any child mutation; sealing this record cannot allocate.
  auto* family = new (inverter.pool.alloc(sizeof(NestedPath::Family), alignof(NestedPath::Family)))
      NestedPath::Family{inverter.currDoc, path.rows};
  assert(!path.last || path.last->parent < family->parent);
  if (path.last) path.last->next = family;
  else path.first = family;
  path.last = family;
  auto seal = scope_guard([&] { family->end = path.rows; });
  size_t index = 0;
  auto object = [&](const IndexVal& child) {
    auto* doc = std::get_if<api::Map>(&child.kind);
    if (!doc && !coerce::isNull(child)) {
      throw DocumentError("Nested path '" + std::string(std::string_view(path.name)) + "' element "
          + std::to_string(index) + " requires an object");
    }
    if (path.depth > MAX_DEPTH) {
      throw DocumentError("Nested object depth exceeds " + std::to_string(MAX_DEPTH) + ": "
          + std::string(std::string_view(path.name)));
    }
    if (!Inverter::rowsFit((uint64_t)path.rows, 1)) {
      throw DocumentError("Nested row limit reached: " + std::string(std::string_view(path.name)));
    }
    if (path.ordinal == SpaceOrdinal::ROOT) path.ordinal = inverter.postingsWriter.addSpace(std::string_view(path.name));
    int32_t savedDoc = inverter.currDoc;
    auto savedSpace = inverter.inputSpace;
    auto* savedPath = active;
    auto restore = scope_guard([&] {
      inverter.currDoc = savedDoc;
      inverter.inputSpace = savedSpace;
      active = savedPath;
    });
    inverter.currDoc = path.rows++;
    inverter.maxChildRows = std::max(inverter.maxChildRows, path.rows);
    inverter.inputSpace = path.ordinal;
    active = &path;
    try {
      if (doc) path.fields.index(inverter, *doc, NestedPath::Names(inverter, path));
    } catch (const DocumentError& e) {
      throw DocumentError(fmt::format("Nested '{}' element {}: {}", std::string_view(path.name), index, e.what()), e.code);
    } catch (const RequestError& e) {
      throw DocumentError(fmt::format("Nested '{}' element {}: {}", std::string_view(path.name), index, e.what()), e.code);
    }
    index++;
  };
  if (array) coerce::forEachElement(val, object);
  else object(val);
}

void NestedState::finish() {
  assert(active == nullptr);
  auto& writer = inverter.postingsWriter;
  std::vector<const NestedPath*> ordered;
  ordered.reserve(paths.size());
  for (const auto& [_, path] : paths) {
    if (path->rows != 0) ordered.push_back(path.get());
  }
  std::sort(ordered.begin(), ordered.end(), [](auto* a, auto* b) { return a->ordinal < b->ordinal; });
  for (const auto* path : ordered) {
    int32_t parents = path->parent ? path->parent->rows : inverter.getMaxDoc();
    size_t blocks = ((size_t)parents + NumColumnFormat::BLOCK_SIZE - 1) / NumColumnFormat::BLOCK_SIZE;
    inverter.noteFlushWorkspace(sizeof(MonoWriter) + 2 * NumColumnFormat::BLOCK_SIZE * sizeof(int64_t)
        + 2 * blocks * (sizeof(NumBlockInfo) + sizeof(NumBlockZone)));
    auto out = writer.getOutputStream();
    MonoWriter ends(writer.pool, *out);
    auto* event = path->first;
    int32_t end = 0;
    for (int32_t row = 0; row < parents; row++) {
      if (event && event->parent == row) {
        end = event->end;
        event = event->next;
      }
      ends.addInt64(end);
    }
    assert(event == nullptr && end == path->rows);
    ends.finish();
    writer.finishSpace(path->ordinal, path->rows, {ends.blockLoc, ends.metaOff, {}, 0});
  }
}

} // namespace luxir
