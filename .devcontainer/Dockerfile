# Copyright 2020-2026 Yonik Seeley and Luxir contributors
# SPDX-License-Identifier: Apache-2.0

FROM ubuntu:22.04@sha256:829f6df217bcbae2b371026e81711d1a787c61b2967ad09d015063663ebafbf7 AS toolchain

ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
      binutils build-essential ca-certificates ccache curl file gdb git \
      ninja-build pkg-config python3 tar tzdata unzip zip xz-utils \
    && rm -rf /var/lib/apt/lists/*

# Compiler Explorer's Ubuntu 22.04 build. Keep this digest when mirroring it.
ARG GCC_VERSION=16.2.0
ARG GCC_SHA256=7bffbfc6b2eb997a7a6e3cbd013372b41de5746850ded8b6453f9820e5678828
RUN curl -fL --retry 3 \
      "https://s3.amazonaws.com/compiler-explorer/opt/gcc-${GCC_VERSION}.tar.xz" \
      -o /tmp/gcc.tar.xz \
    && echo "${GCC_SHA256}  /tmp/gcc.tar.xz" | sha256sum -c - \
    && tar -xJf /tmp/gcc.tar.xz -C /opt \
    && ln -s "gcc-${GCC_VERSION}" /opt/gcc \
    && echo /opt/gcc/lib64 > /etc/ld.so.conf.d/luxir-gcc.conf \
    && ldconfig \
    && rm /tmp/gcc.tar.xz

# Match the CMake version pinned by the selected vcpkg checkout.
ARG CMAKE_VERSION=4.4.3
ARG CMAKE_SHA512=24a4299dfe25d09a89879218f03c9471c2fbe38763820ac19378aa7d12b43ae9d7777628364aeae759b7117d81fe95c9c30c8fe2d36bf89b52b9884f810444ad
RUN curl -fL --retry 3 \
      "https://github.com/Kitware/CMake/releases/download/v${CMAKE_VERSION}/cmake-${CMAKE_VERSION}-linux-x86_64.tar.gz" \
      -o /tmp/cmake.tar.gz \
    && echo "${CMAKE_SHA512}  /tmp/cmake.tar.gz" | sha512sum -c - \
    && mkdir /opt/cmake \
    && tar -xzf /tmp/cmake.tar.gz -C /opt/cmake --strip-components=1 \
    && rm /tmp/cmake.tar.gz

ENV PATH="/opt/gcc/bin:/opt/cmake/bin:${PATH}"
ENV CC=gcc CXX=g++ TZ=Etc/UTC LANG=C.UTF-8

COPY .devcontainer/check-toolchain.cpp .devcontainer/check-toolchain.sh /opt/luxir-checks/
RUN bash /opt/luxir-checks/check-toolchain.sh

WORKDIR /workspace/luxir
CMD ["bash"]

FROM toolchain AS normal-dependencies
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
ENV VCPKG_BINARY_SOURCES="clear;files,/var/cache/vcpkg,readwrite"
# Validate C LAPACK before building the complete dependency set.
RUN --mount=type=cache,target=/opt/vcpkg/downloads \
    --mount=type=cache,target=/var/cache/vcpkg \
    /opt/vcpkg/vcpkg install --classic lapack:x64-linux-luxir-v2 openblas[dynamic-arch]:x64-linux-luxir-v2 \
      --host-triplet=x64-linux-luxir-v2 \
      --overlay-ports=/opt/luxir-deps/ports --overlay-triplets=/opt/luxir-deps/triplets \
      --clean-buildtrees-after-build --clean-packages-after-build

COPY .devcontainer/check-lapack.cpp /opt/luxir-checks/
RUN g++ -std=c++26 -O2 /opt/luxir-checks/check-lapack.cpp \
      /opt/vcpkg/installed/x64-linux-luxir-v2/lib/libopenblas.a \
      -lm -pthread -static-libstdc++ -static-libgcc -o /tmp/check-lapack \
    && /tmp/check-lapack && rm /tmp/check-lapack

RUN --mount=type=cache,target=/opt/vcpkg/downloads \
    --mount=type=cache,target=/var/cache/vcpkg \
    /opt/vcpkg/vcpkg install --x-manifest-root=/opt/luxir-deps \
      --x-install-root=/opt/vcpkg/installed --triplet=x64-linux-luxir-v2 \
      --host-triplet=x64-linux-luxir-v2 \
      --clean-buildtrees-after-build --clean-packages-after-build

FROM normal-dependencies AS dependencies
RUN --mount=type=cache,target=/opt/vcpkg/downloads \
    --mount=type=cache,target=/var/cache/vcpkg \
    /opt/vcpkg/vcpkg install --x-manifest-root=/opt/luxir-deps \
      --x-install-root=/opt/vcpkg/installed-asan --triplet=x64-linux-luxir-v2-asan \
      --host-triplet=x64-linux-luxir-v2 \
      --clean-buildtrees-after-build --clean-packages-after-build

FROM toolchain AS dev
COPY --from=dependencies /opt/vcpkg /opt/vcpkg
COPY --from=dependencies /opt/luxir-deps /opt/luxir-deps
ENV VCPKG_ROOT=/opt/vcpkg
RUN apt-get update && apt-get install -y --no-install-recommends wamerican \
    && rm -rf /var/lib/apt/lists/* \
    && groupadd --gid 1000 dev && useradd --uid 1000 --gid dev --create-home --shell /bin/bash dev
