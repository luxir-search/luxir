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
    platform: str = "linux"

    def package_name(self, version, tier, suffix):
        level = "-" + tier.rsplit("-", 1)[1] if self.isa_notes else ""
        return f"luxir-{version}-{self.platform}-{self.asset}{level}{suffix}"


ARCHITECTURES = (
    Architecture("arm64", "arm64", "", ("apple-m1",),
                 "Requires Apple silicon.\n", platform="macos"),
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


def macos_baseline(root):
    import re
    return re.search(r'set\(LUXIR_MACOS_DEPLOYMENT_TARGET ([0-9.]+)\)',
                     (root / 'deps/triplets/common/macos-version.cmake').read_text())[1]


def macho_libraries(listing):
    import re
    libraries = re.findall(r'^\s+(\S+) \(compatibility version', listing, re.M)
    allowed = {'/usr/lib/libSystem.B.dylib', '/usr/lib/libiconv.2.dylib',
               '/usr/lib/libresolv.9.dylib',
               '/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation',
               '/System/Library/Frameworks/Accelerate.framework/Versions/A/Accelerate'}
    if not libraries or set(libraries) - allowed:
        raise ValueError(f'Unexpected Mach-O runtime libraries: {libraries}')
    return libraries


def macho_check(path, baseline, signed=False):
    import re
    def run(*args):
        return subprocess.check_output(args, text=True).strip()
    if run('lipo', '-archs', str(path)) != 'arm64':
        raise ValueError('Expected a thin arm64 Mach-O executable')
    commands = run('otool', '-l', str(path))
    versions = re.findall(r'\bminos ([0-9.]+)', commands)
    if versions != [baseline]:
        raise ValueError(f'Expected macOS {baseline}, found {versions}')
    libraries = macho_libraries(run('otool', '-L', str(path)))
    if signed:
        subprocess.run(['codesign', '--verify', '--strict', str(path)], check=True)
        signature = subprocess.check_output(['codesign', '-d', '--verbose=4', str(path)],
                                            stderr=subprocess.STDOUT, text=True)
        if 'Signature=adhoc' not in signature:
            raise ValueError('Expected an ad-hoc signature')
    return libraries
