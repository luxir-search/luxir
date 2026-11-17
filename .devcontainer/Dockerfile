# Copyright 2020-2026 Yonik Seeley and Luxir contributors
# SPDX-License-Identifier: Apache-2.0

# deps-image.yml builds the toolchain and the two dependency stages in separate
# jobs, each within a hosted job's time limit, and names the published earlier
# stages here. A local build uses the stages below.
ARG TOOLCHAIN=toolchain
ARG NORMAL_DEPENDENCIES=normal-dependencies

FROM ubuntu:22.04@sha256:829f6df217bcbae2b371026e81711d1a787c61b2967ad09d015063663ebafbf7 AS base

ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
      binutils build-essential ca-certificates ccache curl file gdb git \
      ninja-build pkg-config python3 tar tzdata unzip zip xz-utils \
    && rm -rf /var/lib/apt/lists/*

# Compiler Explorer's Ubuntu 22.04 build. Keep this digest when mirroring it.
FROM base AS gcc-amd64
ARG GCC_VERSION=16.2.0
ARG GCC_SHA256=7bffbfc6b2eb997a7a6e3cbd013372b41de5746850ded8b6453f9820e5678828
RUN curl -fL --retry 3 "https://s3.amazonaws.com/compiler-explorer/opt/gcc-${GCC_VERSION}.tar.xz" -o /tmp/gcc.tar.xz \
    && echo "${GCC_SHA256}  /tmp/gcc.tar.xz" | sha256sum -c - \
    && tar -xJf /tmp/gcc.tar.xz -C /opt && rm /tmp/gcc.tar.xz
ENV LUXIR_CONTAINER_TRIPLET=x64-linux-luxir-v2 LUXIR_CONTAINER_ASAN_TRIPLET=x64-linux-luxir-v2-asan LUXIR_CONTAINER_CPU=x86-64-v2

# Compiler Explorer publishes no ARM64-hosted compiler; build the same release.
# Like that x86-64 build, this one does not default to PIE.
FROM base AS gcc-arm64
ARG GCC_VERSION=16.2.0
# https://gcc.gnu.org/pub/gcc/releases/gcc-16.2.0/sha512.sum
ARG GCC_SOURCE_SHA512=c51c30ca7422d0cbecf504b2e0f33c3aca31e0f90a76b65217f465163fa6fa17b3f5de39e145c47e5bab90ac0ce7fff3b03c8d553ae36e01faaea5a50f8648d1
ARG BUILD_JOBS=12
RUN apt-get update && apt-get install -y --no-install-recommends libgmp-dev libmpfr-dev libmpc-dev flex bison \
    && rm -rf /var/lib/apt/lists/*
RUN curl -fL --retry 3 "https://gcc.gnu.org/pub/gcc/releases/gcc-${GCC_VERSION}/gcc-${GCC_VERSION}.tar.xz" -o /tmp/gcc.tar.xz \
    && echo "${GCC_SOURCE_SHA512}  /tmp/gcc.tar.xz" | sha512sum -c - \
    && mkdir /tmp/gcc-src /tmp/gcc-build \
    && tar -xJf /tmp/gcc.tar.xz -C /tmp/gcc-src --strip-components=1 \
    && cd /tmp/gcc-build \
    && /tmp/gcc-src/configure --prefix=/opt/gcc-${GCC_VERSION} \
      --enable-languages=c,c++ --disable-multilib --disable-bootstrap \
      --enable-multiarch --enable-clocale=gnu --enable-libstdcxx-time=yes \
      --enable-linker-build-id --enable-lto --enable-plugins --enable-threads=posix \
      --with-linker-hash-style=gnu --enable-checking=release \
      --enable-libsanitizer --enable-libgomp --with-arch=armv8-a --with-tune=generic \
      --libdir=/opt/gcc-${GCC_VERSION}/lib64 \
    && make -j"${BUILD_JOBS}" && make install-strip \
    && test -f /opt/gcc-${GCC_VERSION}/lib64/libgomp.a \
    && rm -rf /tmp/gcc-src /tmp/gcc-build /tmp/gcc.tar.xz
ENV LUXIR_CONTAINER_TRIPLET=arm64-linux-luxir-v8 LUXIR_CONTAINER_ASAN_TRIPLET=arm64-linux-luxir-v8-asan LUXIR_CONTAINER_CPU=armv8-a

# BuildKit selects only the native compiler stage; no emulation or cross build.
FROM gcc-${TARGETARCH} AS toolchain
ARG TARGETARCH
ARG GCC_VERSION=16.2.0
# The name sorts ahead of the distribution's entries, so build tools linked
# to the shared libstdc++ load this compiler's, not Ubuntu's older one.
RUN ln -s "gcc-${GCC_VERSION}" /opt/gcc \
    && echo /opt/gcc/lib64 > /etc/ld.so.conf.d/00-luxir-gcc.conf && ldconfig

ARG CMAKE_VERSION=4.4.3
RUN case "$TARGETARCH" in \
      amd64) archive=x86_64; checksum=d6c83076c575bc00b823522ac974bda66d0af05d6ddc30e739c12385cf32c6cc ;; \
      arm64) archive=aarch64; checksum=2efc974dbd63b4444c0e8494b92f2e80c2d7e635b4b80eac2916985ddd8f72a6 ;; \
      *) exit 1 ;; esac \
    && curl -fL --retry 3 "https://github.com/Kitware/CMake/releases/download/v${CMAKE_VERSION}/cmake-${CMAKE_VERSION}-linux-${archive}.tar.gz" -o /tmp/cmake.tar.gz \
    && echo "${checksum}  /tmp/cmake.tar.gz" | sha256sum -c - \
    && mkdir /opt/cmake \
    && tar -xzf /tmp/cmake.tar.gz -C /opt/cmake --strip-components=1 && rm /tmp/cmake.tar.gz

ENV PATH="/opt/gcc/bin:/opt/cmake/bin:${PATH}"
ENV CC=gcc CXX=g++ TZ=Etc/UTC LANG=C.UTF-8

COPY .devcontainer/check-toolchain.cpp .devcontainer/check-toolchain.sh /opt/luxir-checks/
RUN bash /opt/luxir-checks/check-toolchain.sh

WORKDIR /workspace/luxir
CMD ["bash"]

FROM ${TOOLCHAIN} AS normal-dependencies
COPY deps/vcpkg.json deps/vcpkg-configuration.json /opt/luxir-deps/
RUN revision=$(python3 -c 'import json; print(json.load(open("/opt/luxir-deps/vcpkg.json"))["builtin-baseline"])') \
    && git init /opt/vcpkg \
    && git -C /opt/vcpkg remote add origin https://github.com/microsoft/vcpkg.git \
    && git -C /opt/vcpkg fetch --depth 1 origin "$revision" \
    && git -C /opt/vcpkg checkout --detach FETCH_HEAD \
    && /opt/vcpkg/bootstrap-vcpkg.sh -disableMetrics

COPY deps/ports /opt/luxir-deps/ports
COPY deps/triplets /opt/luxir-deps/triplets
ARG BUILD_JOBS=12
ENV VCPKG_MAX_CONCURRENCY=${BUILD_JOBS} VCPKG_DISABLE_METRICS=1
# Packages are archived twice: in the BuildKit cache, which survives local
# image rebuilds, and in this stage, so a later stage built from the published
# image restores the host tools it shares. The dev stage does not copy them.
ENV VCPKG_BINARY_SOURCES="clear;files,/var/cache/vcpkg,readwrite;files,/var/lib/luxir-vcpkg-archives,readwrite"
RUN mkdir -p /var/lib/luxir-vcpkg-archives
# A failed port leaves its reason in build logs that die with the build
# container, so show them.
RUN printf '%s\n' '#!/bin/sh' \
      '/opt/vcpkg/vcpkg install "$@" && exit' \
      'status=$?' \
      'for log in $(ls -t /opt/vcpkg/buildtrees/*/*.log | head -n 3); do' \
      '  echo "==> $log"; grep -n -m 20 -A 40 -E "FAILED:|[Ee]rror[: ]|Killed" "$log"; tail -n 40 "$log"' \
      'done' \
      'df -h / /opt/vcpkg; free -m' \
      'exit $status' > /usr/local/bin/vcpkg-install \
    && chmod +x /usr/local/bin/vcpkg-install
# Validate C LAPACK before building the complete dependency set.
RUN --mount=type=cache,target=/opt/vcpkg/downloads \
    --mount=type=cache,target=/var/cache/vcpkg \
    vcpkg-install --classic lapack:${LUXIR_CONTAINER_TRIPLET} openblas[dynamic-arch]:${LUXIR_CONTAINER_TRIPLET} \
      --host-triplet=${LUXIR_CONTAINER_TRIPLET} \
      --overlay-ports=/opt/luxir-deps/ports --overlay-triplets=/opt/luxir-deps/triplets \
      --clean-buildtrees-after-build --clean-packages-after-build

COPY test/blas_lapack_test.cpp /opt/luxir-checks/check-lapack.cpp
RUN g++ -std=c++26 -O2 /opt/luxir-checks/check-lapack.cpp \
      /opt/vcpkg/installed/${LUXIR_CONTAINER_TRIPLET}/lib/libopenblas.a \
      -lm -pthread -static-libstdc++ -static-libgcc -o /tmp/check-lapack \
    && /tmp/check-lapack && rm /tmp/check-lapack

RUN --mount=type=cache,target=/opt/vcpkg/downloads \
    --mount=type=cache,target=/var/cache/vcpkg \
    vcpkg-install --x-manifest-root=/opt/luxir-deps \
      --x-install-root=/opt/vcpkg/installed --triplet=${LUXIR_CONTAINER_TRIPLET} \
      --host-triplet=${LUXIR_CONTAINER_TRIPLET} --x-feature=jemalloc \
      --clean-buildtrees-after-build --clean-packages-after-build

FROM ${NORMAL_DEPENDENCIES} AS dependencies
RUN --mount=type=cache,target=/opt/vcpkg/downloads \
    --mount=type=cache,target=/var/cache/vcpkg \
    vcpkg-install --x-manifest-root=/opt/luxir-deps \
      --x-install-root=/opt/vcpkg/installed-asan --triplet=${LUXIR_CONTAINER_ASAN_TRIPLET} \
      --host-triplet=${LUXIR_CONTAINER_TRIPLET} \
      --clean-buildtrees-after-build --clean-packages-after-build

FROM ${TOOLCHAIN} AS dev
COPY --from=dependencies /opt/vcpkg /opt/vcpkg
COPY --from=dependencies /opt/luxir-deps /opt/luxir-deps
ENV VCPKG_ROOT=/opt/vcpkg
RUN apt-get update && apt-get install -y --no-install-recommends wamerican \
    && rm -rf /var/lib/apt/lists/* \
    && groupadd --gid 1000 dev && useradd --uid 1000 --gid dev --create-home --shell /bin/bash dev
