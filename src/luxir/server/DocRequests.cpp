// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "DocRequests.h"

#include "luxir/api/build.h"
#include "luxir/util/ApiError.h"

#include <algorithm>

namespace luxir {

void getAsSearch(const api::GetRequest& get, api::SearchRequest& out, std::pmr::memory_resource& mr) {
  if (get.ids.empty()) throw RequestError("get: ids must not be empty");
  out.request_id = get.request_id;
  out.collection = get.collection;
  out.min_commit = get.min_commit;
  out.min_commit_timeout_ms = get.min_commit_timeout_ms;

  auto* values = api::build::allocMessage<api::Val>(mr);
  values->kind = api::ArrStr{.v = get.ids};
  auto* query = api::build::allocMessage<api::Query>(mr);
  query->kind = api::AnyOfQuery{.field = "id", .values = values};

  auto* op = api::build::mapSlot<api::SearchOp>(out.ops, 1, GET_OP, mr);
  api::TopDocs topDocs;
  topDocs.query = query;
  topDocs.limit = (int64_t)get.ids.size();
  // Results come back in index order, so an explicit field list always
  // includes id to map them back.
  topDocs.fields = get.fields;
  if (!get.fields.empty() && std::ranges::find(get.fields, std::string_view("id")) == get.fields.end()) {
    std::span<const std::string_view> withId;
    auto* fields = api::build::allocArray(withId, get.fields.size() + 1, mr);
    fields[0] = "id";
    std::ranges::copy(get.fields, fields + 1);
    topDocs.fields = withId;
  }
  op->kind = topDocs;
}

void deleteAsUpdate(const api::DeleteRequest& del, api::UpdateRequest& out) {
  if (del.ids.empty()) throw RequestError("delete: ids must not be empty");
  out.request_id = del.request_id;
  out.collection = del.collection;
  out.delete_ids = del.ids;
  out.commit = del.commit;
}

} // namespace luxir
