// Descriptor-only reflection: register luxir's schema into the protobuf
// generated_pool() from an embedded FileDescriptorSet, with NO generated message
// or service classes. Call once at server startup; grpc's default reflection
// plugin then serves the schema (FileContainingSymbol / describe) unchanged.
#pragma once

namespace luxir {

// Idempotent. Registers every luxir file not already present in generated_pool()
// (the well-known types are already there via libprotobuf).
void registerLuxirDescriptors();

// Test/health helper: true iff the registered descriptors resolve the services and
// a representative message. Returns bool (no protobuf types) so hpp-only TUs can call
// it without pulling protobuf headers into the same TU as the hpp serializer.
bool luxirSchemaResolves();

} // namespace luxir
