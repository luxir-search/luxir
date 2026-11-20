// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0
#include "SchemaCache.h"
#include "luxir/store/Manifest.h"

namespace luxir {

SchemaCache::Key SchemaCache::key(std::string_view incarnation, const SchemaBinding& binding) {
  return {binding.scope, binding.name, binding.version.local ? std::string(incarnation) : std::string{}, binding.version};
}

std::shared_ptr<Schema> SchemaCache::remember(const Key& key, std::shared_ptr<Schema> schema) {
  std::lock_guard lock(mutex);
  if (auto it = entries.find(key); it != entries.end()) {
    if (auto cached = it->second.lock()) {
      if (cached->file != schema->file) throw std::runtime_error("Schema version has different contents");
      return cached;
    }
  }
  struct Held {
    std::shared_ptr<Schema> schema;
    StorageCharge charge;
  };
  auto bytes = schema->estimatedBytes();
  auto held = std::make_shared<Held>(schema, StorageCharge(memory->child(schema->binding.scope), bytes));
  auto result = std::shared_ptr<Schema>(held, schema.get());
  entries[key] = result;
  if (++additions >= std::max((size_t)64, entries.size())) {
    std::erase_if(entries, [](const auto& e) { return e.second.expired(); });
    additions = 0;
  }
  return result;
}

std::shared_ptr<Schema> SchemaCache::load(Directory& dir, const api::IndexInfo& info) {
  if (!info.schema) throw std::runtime_error("Missing schema reference");
  SchemaBinding binding(*info.schema);
  // Shared resolution will select the schema's tenant/name directory. This
  // format already carries that binding, but no shared publisher exists yet.
  if (!binding.name.empty() || !binding.version.local || !binding.version.epoch)
    throw std::runtime_error("Unsupported schema reference");
  const auto& desc = info.schema_file;
  if (desc.name != SchemaBinding::fileName(binding.version)) throw std::runtime_error("Invalid schema file reference");
  auto location = key(info.incarnation, binding);
  {
    std::lock_guard lock(mutex);
    if (auto it = entries.find(location); it != entries.end()) {
      if (auto schema = it->second.lock()) {
        if (schema->file.size != desc.size || schema->file.xxh3 != desc.xxh3)
          throw std::runtime_error("Schema version has different contents");
        return schema;
      }
    }
  }
  auto file = dir.openFile(desc.name);
  if (!file) throw std::runtime_error("Missing schema file: " + std::string(desc.name));
  auto body = file->read();
  if (body.size() != desc.size || body.size() < 16 || XXH3_64bits(body.data(), body.size()) != desc.xxh3)
    throw std::runtime_error("Invalid schema file: " + std::string(desc.name));
  auto input = file->getInputStream();
  auto length = body.size() - 16;
  input.seek(length);
  if ((uint64_t)input.readLong() != length || (uint64_t)input.readLong() != XXH3_64bits(body.data(), length))
    throw std::runtime_error("Invalid schema footer: " + std::string(desc.name));
  std::pmr::monotonic_buffer_resource arena;
  api::SchemaInfo stored;
  if (!api::decode(stored, api::copyToPaddedInput(std::as_bytes(std::span(body.data(), length)), arena), arena))
    throw std::runtime_error("Invalid stored schema");
  auto schema = Schema::fromStored(stored, std::move(binding));
  schema->file = {std::string(desc.name), desc.size, desc.xxh3};
  return remember(location, std::move(schema));
}

std::shared_ptr<Schema> SchemaCache::publish(Directory& dir, std::string_view incarnation, std::shared_ptr<Schema> schema) {
  std::pmr::monotonic_buffer_resource arena;
  std::vector<std::byte> bytes;
  if (!api::encode(schema->storedInfo(arena), bytes)) throw std::runtime_error("Cannot encode schema");
  auto contents = Manifest::withFooter(bytes);
  auto name = SchemaBinding::fileName(schema->binding.version);
  if (!dir.publishFile(name, contents, Directory::PublishMode::CREATE_ONLY)) {
    auto existing = dir.openFile(name);
    auto body = existing ? existing->read() : std::string_view{};
    if (body.size() != contents.size() || memcmp(body.data(), contents.data(), contents.size()))
      throw std::runtime_error("Schema version already contains different bytes");
    std::array<std::string, 1> names{name};
    dir.sync(names);
  }
  schema->file = {name, contents.size(), XXH3_64bits(contents.data(), contents.size())};
  auto location = key(incarnation, schema->binding);
  return remember(location, std::move(schema));
}

}
