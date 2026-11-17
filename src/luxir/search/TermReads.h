// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "luxir/api/luxir_types.hpp"
#include "luxir/store/FetchCountdown.h"

namespace luxir {

class IndexReader;
class Schema;

/// The reads that parsing `request` will make in term dictionaries, declared
/// before it parses, so that a search can fetch them without holding a
/// thread and then parse on resident bytes.
///
/// Parsing seeks terms (and expands prefixes, fuzzy terms and patterns) per
/// segment, and a seek opens the segment's base. This declares the base reads
/// of every segment not open yet where a term-level query (match, any_of,
/// phrase, prefix, fuzzy, wildcard, regex, multi_match, through boolean,
/// boost, constant_score, rescore and dis_max) names a TEXT, STRING or ID
/// field that may be present. An exact STRING or ID value skips the segments
/// whose summary proves it absent, as the seek does.
///
/// Only a fetch depends on this, never a result: a read it misses blocks in
/// parse as before, and one it adds costs bytes. Fields are taken by their
/// physical name; string queries (simple_query, expr), fusion and kNN sources
/// and field defaults are not resolved here, so their seeks still block.
void declareTermReads(const api::SearchRequest& request, const Schema& schema, IndexReader& reader,
                      ReadSet& reads);

}  // namespace luxir
