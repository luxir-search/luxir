// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Hooks the example plugin exports to its tests.

void exampleSetOverlayOpenMode(int mode);
// example_absent_prefix probe batches issued since the last reset.
int exampleMembershipProbeBatches();
void exampleResetMembershipProbeBatches();
