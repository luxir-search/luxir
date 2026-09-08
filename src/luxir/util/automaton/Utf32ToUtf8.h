// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include "luxir/util/automaton/Automaton.h"
namespace luxir::automaton {
Automaton utf32ToUtf8(const Automaton& utf32, Budget& budget);
}
