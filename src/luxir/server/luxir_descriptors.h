// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

// Descriptor-only reflection: register luxir's schema into the protobuf
// generated_pool() from an embedded FileDescriptorSet, with NO generated message
// or service classes. Call once at server startup; grpc's default reflection
// plugin then serves the schema (FileContainingSymbol / describe) unchanged.
#pragma once

namespace luxir {

// Idempotent. Registers every luxir file not already present in generated_pool()
// (the well-known types are already there via libprotobuf).
void registerLuxirDescriptors();

} // namespace luxir
