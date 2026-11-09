// Copyright 2026 Luxir contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "luxir/search/Collector.h"
namespace luxir {
void emitInnerHits(IndexReader& reader, std::span<Query::SegmentSource* const> sources,
    std::span<const segdoc> docs, api::DocList& output, std::pmr::memory_resource& mr);
}
