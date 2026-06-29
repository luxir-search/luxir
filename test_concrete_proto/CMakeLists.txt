# Concrete-class round-trip tests (Phase 2 cutover). Both targets are EXCLUDE_FROM_ALL;
# nothing here is built by `all` or by solux_test. Build on demand:
#     cmake --build --preset gcc-debug --target solux_concrete_sweep
#     cmake --build --preset gcc-debug --target solux_concrete_build_test
#
# The canonical concrete library (out-of-line pb_meta/glz::meta metadata bound to the
# hand-written src/solux/api headers) is solux_proto_concrete, defined in protos/. We just
# link it here; its PUBLIC include dir brings in src/solux/api (the concrete headers +
# build.h) and its PUBLIC deps bring hpp_proto::runtime + glaze.

# Phase 1 validation sweep: build representative instances, round-trip binary + JSON.
add_executable(solux_concrete_sweep EXCLUDE_FROM_ALL sweep_test.cpp)
target_link_libraries(solux_concrete_sweep PRIVATE solux_proto_concrete)
set_target_properties(solux_concrete_sweep PROPERTIES CXX_STANDARD 26 CXX_STANDARD_REQUIRED ON)
add_test(NAME solux_concrete_sweep COMMAND solux_concrete_sweep)

# Build-by-backing proof: assemble a representative non-owning SearchResponse with the
# src/solux/api/build.h helpers and round-trip it (binary + JSON). De-risks the
# response-assembly primitives before the engine cutover.
add_executable(solux_concrete_build_test EXCLUDE_FROM_ALL build_test.cpp)
target_link_libraries(solux_concrete_build_test PRIVATE solux_proto_concrete)
set_target_properties(solux_concrete_build_test PROPERTIES CXX_STANDARD 26 CXX_STANDARD_REQUIRED ON)
add_test(NAME solux_concrete_build_test COMMAND solux_concrete_build_test)
