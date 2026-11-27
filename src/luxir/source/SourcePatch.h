// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Partial updates of a stored source: a JSON document edited in place.
//
// The document is kept as its own bytes. Only the objects and arrays an
// operation descends into are split into members; everything else, including
// every value the patch does not touch, is written back byte for byte, so key
// order, duplicate keys and number spelling survive a patch. New values are
// written as canonical JSON.

#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "luxir/api/luxir_types.hpp"

namespace luxir::source {

// A patch that cannot apply to this document (a type mismatch, an overflow, a
// path through a non-object). The document is left unchanged.
class PatchError : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

class PatchDoc {
public:
  struct Node {
    enum Kind : uint8_t { RAW, OBJECT, ARRAY };
    Kind kind = RAW;
    std::string_view raw;  // RAW: the value's JSON text
    // OBJECT: members in document order; keys are JSON string tokens, quotes included.
    std::vector<std::pair<std::string_view, Node>> members;
    std::vector<Node> items;  // ARRAY
  };

  // `json` must be a JSON object. It is copied.
  explicit PatchDoc(std::string_view json);

  // The value at `path`, or null when absent. The last of duplicate keys wins.
  const Node* get(std::string_view path);
  // JSON text of a value.
  std::string text(const Node& node) const;
  std::string serialize() const { return text(root); }

  void set(std::string_view path, std::string_view json);
  void unset(std::string_view path);
  void merge(std::string_view path, std::string_view json);
  void replace(std::string_view json);
  enum class Arith { ADD, MULTIPLY, MIN, MAX };
  void arith(Arith op, std::string_view path, std::string_view json);
  void append(std::string_view path, std::string_view json, bool unique);
  void removeValues(std::string_view path, std::string_view json);

private:
  std::vector<std::unique_ptr<std::string>> owned;
  Node root;

  std::string_view own(std::string text);
  void expand(Node& node);
  // The object holding the last path segment, creating missing objects when
  // `create`; null when absent. Sets `key` to the last segment.
  Node* parent(std::string_view path, bool create, std::string_view& key);
  void mergeInto(Node& target, std::string_view patchJson);
  void putMember(Node& object, std::string_view key, Node value);
};

// Three-way comparison of two JSON values: numbers by value, strings by their
// decoded text, booleans false < true, null equal to null; objects and arrays
// only by equality. Nullopt when the values are not comparable.
std::optional<int> compareJson(std::string_view a, std::string_view b);
bool equalJson(std::string_view a, std::string_view b);

// The JSON text of a request value.
std::string valJson(const api::Val& val);

// Whether a patch path may be written: not empty, not "id" or under it, and
// no segment a reserved "_name_" field. Nullopt when valid, else the reason.
std::optional<std::string> invalidPatchPath(std::string_view path);

struct PatchOutcome {
  api::PatchResult::Status status = api::PatchResult::Status::UNKNOWN;
  std::string body;   // UPDATED, CREATED: the new document
  std::string error;  // ERROR
};

// Applies `patch` to the document `base` (JSON; nullopt when there is none)
// whose _version_ is `baseVersion`.
PatchOutcome applyPatch(const api::Patch& patch, const std::optional<std::string>& base, uint64_t baseVersion);

}  // namespace luxir::source
