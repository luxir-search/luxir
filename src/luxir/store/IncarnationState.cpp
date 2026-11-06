// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "IncarnationState.h"
#include <array>
#include <glaze/glaze.hpp>
#include "luxir/util/Uuid.h"
#include "luxir/util/Signal.h"

namespace luxir {
namespace {
void durable(Directory& dir) {
  std::array<std::string, 1> names{"STATE"};
  dir.sync(names);
}
std::optional<IncarnationState> write(Directory& dir, const IncarnationState::Value& value,
                                     Directory::PublishCondition condition) {
  auto bytes = glz::write_json(value).value();
  try {
    if (auto version = dir.publishFile("STATE", std::as_bytes(std::span(bytes)), condition)) {
      Signal::emit("statePublished", &dir);
      return IncarnationState{value, *version};
    }
  } catch (...) {
    auto failure = std::current_exception();
    auto observed = dir.readPublished("STATE");
    if (observed && observed->bytes == bytes) {
      durable(dir);
      return IncarnationState{value, observed->version};
    }
    std::rethrow_exception(failure);
  }
  return {};
}
void increment(uint64_t& value) {
  if (value == UINT64_MAX) throw std::runtime_error("STATE counter exhausted");
  value++;
}
}

std::optional<IncarnationState> IncarnationState::read(Directory& dir) {
  auto publication = dir.readPublished("STATE");
  if (!publication) return {};
  Value value;
  if (glz::read_json(value, publication->bytes) || !value.rev || value.owner.empty()) {
    throw std::runtime_error("invalid incarnation STATE");
  }
  return IncarnationState{std::move(value), std::move(publication->version)};
}

IncarnationState IncarnationState::claim(Directory& dir) {
  auto owner = newUuid();
  for (;;) {
    auto before = read(dir);
    if (!before) throw WriterSupersededError();
    auto next = before->value;
    increment(next.rev);
    increment(next.epoch);
    next.owner = owner;
    if (auto applied = write(dir, next, Directory::PublishCondition::match(before->version))) return *applied;
  }
}

void IncarnationState::advance(Directory& dir, std::string_view tip) {
  auto next = value;
  increment(next.rev);
  next.tip = tip;
  std::exception_ptr failure;
  try {
    if (auto applied = write(dir, next, Directory::PublishCondition::match(version))) { *this = std::move(*applied); return; }
  } catch (...) { failure = std::current_exception(); }
  auto observed = read(dir);
  if (observed && observed->value.tip == tip) {
    durable(dir);
    // A takeover may retain our tip. It proves the commit, but the next
    // publication must still use our old ownership and conflict.
    if (observed->value.epoch == value.epoch && observed->value.owner == value.owner) *this = *observed;
    return;
  }
  if (!observed || observed->value.epoch != value.epoch || observed->value.owner != value.owner) {
    throw WriterSupersededError();
  }
  if (failure) std::rethrow_exception(failure);
  throw std::runtime_error("STATE publication conflict within one writer epoch");
}

void IncarnationState::create(Directory& dir) {
  // A fresh owner makes a recreated STATE byte-distinct, so a stale Match
  // cannot succeed against it even if filesystem metadata is reused.
  Value initial{1, 0, newUuid(), {}};
  if (write(dir, initial, Directory::PublishMode::CREATE_ONLY)) return;
  if (!read(dir)) throw WriterSupersededError();
  durable(dir);
}

void IncarnationState::install(Directory& dir, std::string_view tip) {
  for (;;) {
    auto before = read(dir);
    if (!before) throw WriterSupersededError();
    auto next = before->value;
    if (next.tip == tip) { durable(dir); return; }
    increment(next.rev);
    next.tip = tip;
    if (write(dir, next, Directory::PublishCondition::match(before->version))) return;
  }
}
}
