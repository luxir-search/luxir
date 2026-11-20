vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO facebookresearch/faiss
    REF "v${VERSION}"
    SHA512 f653dafbf1750b8c1719f12c6fa63d3fe84093ba0c663c6fa787ab6e381dab5ce8606830c821df9ad8d5300dfea73d7cfb890437a9e006539c417bffa4f8e799
    HEAD_REF main
    PATCHES
        dispatch-features.diff  # AVX2/AVX-512 levels also need FMA, F16C and POPCNT
        rabitq-negative-l2.diff  # keep negative 1-bit L2 estimates; see vector-search.md
        rabitq-neon.diff  # 1-bit kernels measured faster than upstream on Neoverse N1
)

vcpkg_check_features(OUT_FEATURE_OPTIONS FEATURE_OPTIONS
    FEATURES
        gpu     FAISS_ENABLE_GPU
)

if ("gpu" IN_LIST FEATURES)
    vcpkg_find_cuda(OUT_CUDA_TOOLKIT_ROOT cuda_toolkit_root)
    list(APPEND FEATURE_OPTIONS
        "-DCMAKE_CUDA_COMPILER=${NVCC}"
        "-DCUDAToolkit_ROOT=${cuda_toolkit_root}"
    )
    if(VCPKG_TARGET_IS_WINDOWS)
        # See https://github.com/NVIDIA/cuda-samples/pull/412
        list(APPEND FEATURE_OPTIONS "-DCMAKE_CUDA_FLAGS_INIT=-Xcompiler=/Zc:preprocessor")
    endif()
endif()

# Apple silicon has NEON but no SVE, which the arm64 dd level also builds.
set(opt_level dd)
if(VCPKG_TARGET_IS_OSX AND VCPKG_TARGET_ARCHITECTURE STREQUAL "arm64")
    set(opt_level generic)
endif()

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        ${FEATURE_OPTIONS}
        -DFAISS_ENABLE_MKL=OFF
        -DFAISS_ENABLE_METAL=OFF  # Apple GPU backend; needs Xcode's metal compiler
        -DFAISS_ENABLE_PYTHON=OFF  # Requires SWIG
        -DBUILD_TESTING=OFF
        "-DFAISS_OPT_LEVEL=${opt_level}"
)

vcpkg_cmake_install()
vcpkg_copy_pdbs()
vcpkg_cmake_config_fixup()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
