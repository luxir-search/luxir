list(APPEND VCPKG_HASH_ADDITIONAL_FILES "${CMAKE_CURRENT_LIST_FILE}")
string(APPEND VCPKG_C_FLAGS " -g -fsanitize=address -fno-omit-frame-pointer")
string(APPEND VCPKG_CXX_FLAGS " -g -fsanitize=address -fno-omit-frame-pointer")
set(VCPKG_LINKER_FLAGS "-fsanitize=address")
# Optimized sanitizer builds need line tables for diagnostics. Full variable
# tracking can take minutes per FAISS translation unit; Debug keeps full -g.
string(APPEND VCPKG_C_FLAGS_RELEASE " -g1")
string(APPEND VCPKG_CXX_FLAGS_RELEASE " -g1")
