// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <string_view>
#include "luxir/util/automaton/ByteDfa.h"
#include "luxir/util/automaton/CodepointFolder.h"
#include "luxir/util/automaton/Utf32ToUtf8.h"
namespace luxir::automaton {
// With `caseInsensitive`, each literal (escaped or not) also accepts the case
// alternates of its folded codepoints.
ByteDfa compileWildcard(std::string_view pattern, Budget& budget,
                        const CodepointFolder* folder = nullptr, bool caseInsensitive = false);
}
