# Release tarball: generated configure needs no host autotools, and VERSION
# names the release (GitHub's source archive reports 0.0.0). 5.4.0 needs no GCC 16 patch.
#
# Embed defaults here: je_malloc_conf is weak, so a defining object in luxir_lib
# might not be pulled into an executable. MALLOC_CONF still overrides these.
# disable_large_size_classes:false restores 5.3.0's large size classes and avoids
# 5.4's extra retained-allocation batching when those classes are disabled.
# dirty_decay_ms:1000 (default 10000) returns freed memory sooner.
# background_thread:true services decay even in idle custom arenas; wakeups are
# best effort, so dirty_decay_ms is not a strict idle-return deadline.
# cache_oblivious:false removes the extra page and random offset of large
# allocations, so aligned whole-huge-page buffers have no resident fringe.
# --enable-prof compiles in profiling, off unless enabled (MALLOC_CONF=prof:true,...).
# When off it costs ~0.4% CPU/request; the libgcc unwinder needs no frame pointers.
# --enable-debug adds assertions and junk filling in debug builds.
vcpkg_download_distfile(ARCHIVE
    URLS "https://github.com/jemalloc/jemalloc/releases/download/${VERSION}/jemalloc-${VERSION}.tar.bz2"
    FILENAME "jemalloc-${VERSION}.tar.bz2"
    SHA512 a6771bdc3f066fc458d4723cee3018b445b966a96013074b6aa5ee685e9991ca8a20473ee069c9f4b5912b1ed74a705b059f5c19b4e57c3035d428f055b4d7fd
)
vcpkg_extract_source_archive(SOURCE_PATH
    ARCHIVE "${ARCHIVE}"
)

vcpkg_make_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    DISABLE_MSVC_WRAPPERS
    DISABLE_MSVC_TRANSFORMATIONS
    OPTIONS --enable-prof "--with-malloc-conf=disable_large_size_classes:false,dirty_decay_ms:1000,background_thread:true,cache_oblivious:false"
    OPTIONS_DEBUG --enable-debug
)

vcpkg_make_install()

vcpkg_fixup_pkgconfig()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/share")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/tools")

file(INSTALL "${SOURCE_PATH}/COPYING" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}" RENAME copyright)
