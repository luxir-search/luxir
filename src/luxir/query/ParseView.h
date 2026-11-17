// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "luxir/query/FieldResolver.h"

namespace luxir {

class IndexReader;
class SearchRequest;

// One current view for syntax and lowering. Readers are pinned by the request;
// offline parsing still scopes schema names without opening a reader.
class ParseView {
public:
  FieldResolver fields;

private:
  SearchRequest* request;
  // The searched reader, when there is one: wildcard field lists reuse its
  // schema-specific search catalog.
  const std::shared_ptr<IndexReader>* current;

  void enter(const std::shared_ptr<IndexReader>* reader, std::string_view path) {
    fields.setPath(path);
    current = reader;
  }

public:
  ParseView(Schema& schema, std::vector<std::string>* explanations = nullptr,
            const std::shared_ptr<IndexReader>* reader = nullptr, SearchRequest* request = nullptr);

  IndexReader* get() const { return current ? current->get() : nullptr; }
  const std::shared_ptr<IndexReader>& reader() const { return *current; }
  SearchRequest* owner() const { return request; }

  class Scope {
    ParseView& view;
    const std::shared_ptr<IndexReader>* previous;
    std::string_view path;

  public:
    Scope(ParseView& view, std::string_view target);
    ~Scope() { view.enter(previous, path); }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
  };

  Scope scoped(std::string_view path) { return Scope(*this, path); }
};

} // namespace luxir
