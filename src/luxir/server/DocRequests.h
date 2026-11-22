// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <memory_resource>

#include "luxir/api/luxir_types.hpp"

namespace luxir {

// Get and delete by id, shared by the HTTP and gRPC transports. Both run on the
// existing request paths, so visibility, commit tokens, replica waits and
// errors behave exactly as for searches and updates.

// The name of the top_docs op a get-by-id search answers with.
inline constexpr std::string_view GET_OP = "docs";

// A get becomes a search: one top_docs op (GET_OP) whose query is an any_of
// term lookup on the id field, with room for every id; an explicit field list
// gains "id". Views alias `get`; new
// nodes are allocated from mr. Throws RequestError for an empty id list.
void getAsSearch(const api::GetRequest& get, api::SearchRequest& out, std::pmr::memory_resource& mr);

// A delete becomes an update with only delete_ids. Views alias `del`. Throws
// RequestError for an empty id list.
void deleteAsUpdate(const api::DeleteRequest& del, api::UpdateRequest& out);

} // namespace luxir
