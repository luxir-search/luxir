# Release tarball: generated configure needs no host autotools, and VERSION
# names the release (GitHub's source archive reports 0.0.0). 5.4.0 needs no GCC 16 patch.
#
# disable_large_size_classes:false restores 5.3.0's large classes. With the
# 5.3.1+ default true and cache_oblivious's +4 KiB pad, inverters' 4 MiB MemPool
# blocks come from 5 MiB batches whose tails also get used. After a flush frees
# the pools, ~1400 isolated ~5 MiB dirty extents remain below the 8 MiB oversize
# eager-purge threshold: about 5 GB on the 10M-document corpus until dirty decay.
# With false, freed blocks coalesce and purge on free.
# Embed defaults here: je_malloc_conf is weak, so a defining object in luxir_lib
# might not be pulled into an executable. MALLOC_CONF still overrides these.
# dirty_decay_ms:1000 (default 10000) returns freed memory sooner.
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
    OPTIONS --enable-prof "--with-malloc-conf=disable_large_size_classes:false,dirty_decay_ms:1000"
    OPTIONS_DEBUG --enable-debug
)

vcpkg_make_install()

vcpkg_fixup_pkgconfig()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/share")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/tools")

file(INSTALL "${SOURCE_PATH}/COPYING" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}" RENAME copyright)
