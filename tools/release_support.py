# Copyright 2020-2026 Yonik Seeley and Luxir contributors
# SPDX-License-Identifier: Apache-2.0
"""CPU support as seen by the same glibc loader that rejects our ISA notes."""

import subprocess


def cpu_supported(tier):
    output = subprocess.check_output(
        ["/lib64/ld-linux-x86-64.so.2", "--help"], text=True)
    return any(line.strip() == f"{tier} (supported, searched)"
               for line in output.splitlines())
