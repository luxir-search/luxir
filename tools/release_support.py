# Copyright 2020-2026 Yonik Seeley and Luxir contributors
# SPDX-License-Identifier: Apache-2.0
"""Architecture policy shared by packaging and nightly tests."""

import platform
import subprocess
from dataclasses import dataclass


@dataclass(frozen=True)
class Architecture:
    name: str
    asset: str
    loader: str
    tiers: tuple
    cpu_help: str
    isa_notes: bool = False

    def package_name(self, version, tier, suffix):
        level = "-" + tier.rsplit("-", 1)[1] if self.isa_notes else ""
        return f"luxir-{version}-linux-{self.asset}{level}{suffix}"


ARCHITECTURES = (
    Architecture("amd64", "x86_64", "/lib64/ld-linux-x86-64.so.2",
                 ("x86-64-v2", "x86-64-v3", "x86-64-v4"),
                 "Run /lib64/ld-linux-x86-64.so.2 --help to inspect supported CPU levels.\n", True),
    Architecture("arm64", "aarch64", "/lib/ld-linux-aarch64.so.1", ("armv8-a",),
                 "Requires a little-endian AArch64 CPU with the Armv8-A baseline.\n"),
)


def architecture(tier):
    return next(item for item in ARCHITECTURES if tier in item.tiers)


def cpu_supported(tier):
    arch = architecture(tier)
    if not arch.isa_notes:
        return platform.machine() in ("aarch64", "arm64")
    if platform.machine() != "x86_64":
        return False
    output = subprocess.check_output([arch.loader, "--help"], text=True)
    return any(line.strip() == f"{tier} (supported, searched)"
               for line in output.splitlines())
