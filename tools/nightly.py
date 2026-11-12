#!/usr/bin/env python3
# Copyright 2020-2026 Yonik Seeley and Luxir contributors
# SPDX-License-Identifier: Apache-2.0
"""Nightly planning, advisory tests, and release assembly. No builds in publish.

Usage: nightly.py plan
       nightly.py test PRESET OUTPUT IMAGE_ID
       nightly.py publish DOWNLOAD_DIRECTORY COMMIT IMAGE PRESET [PRESET ...]
Environment: standard GitHub Actions variables and GH_TOKEN for API operations.
"""

import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

sys.dont_write_bytecode = True


TESTS = ("luxir_test", "decoded_successor_no_avx512_test", "decoded_successor_scalar_test")


def run(*args):
    return subprocess.check_output(args, text=True).strip()


def gh(*args):
    return run("gh", *args, "--repo", os.environ["GITHUB_REPOSITORY"])


def api(method, endpoint, data=None, missing_ok=False):
    command = ["gh", "api", "--method", method,
               f"repos/{os.environ['GITHUB_REPOSITORY']}/{endpoint}"]
    if data is not None:
        command += ["--input", "-"]
    result = subprocess.run(command, input=json.dumps(data) if data is not None else None,
                            text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if result.returncode:
        if missing_ok and "(HTTP 404)" in result.stderr:
            return None
        raise RuntimeError(result.stderr)
    return json.loads(result.stdout) if result.stdout.strip() else None


def plan():
    commit = run("git", "rev-parse", "HEAD")
    tags = dict((line.split()[1], line.split()[0]) for line in
                run("git", "ls-remote", "origin", "refs/tags/nightly", "refs/tags/nightly^{}").splitlines())
    previous = tags.get("refs/tags/nightly^{}", tags.get("refs/tags/nightly"))
    build = os.environ["GITHUB_EVENT_NAME"] == "workflow_dispatch" or previous != commit
    outputs = {"commit": commit, "build": str(build).lower(),
               "presets": json.dumps(os.environ["NIGHTLY_PRESETS"].split())}
    if build:
        key = run("python3", "tools/benchmarks/deps-key.py", ".")
        image = f"ghcr.io/{os.environ['GITHUB_REPOSITORY_OWNER'].lower()}/luxir-dev:{key}"
        result = subprocess.run(["timeout", "60", "docker", "manifest", "inspect", image],
                                stdout=subprocess.DEVNULL)
        if result.returncode:
            raise RuntimeError(f"Missing {image}. Publish reviewed dependency inputs with deps-image.yml first.")
        outputs["image"] = image
    with open(os.environ["GITHUB_OUTPUT"], "a") as stream:
        stream.writelines(f"{key}={value}\n" for key, value in outputs.items())
    print(f"Source: {commit}; " + ("build requested" if build else "nightly already current"))


def test(preset, output, image):
    from release_support import cpu_supported

    output = Path(output)
    cache = (Path("build") / preset / "CMakeCache.txt").read_text()
    tier = re.search(r"^LUXIR_CPU_TARGET:[^=]+=(.+)$", cache, re.M)[1]
    supported = cpu_supported(tier)
    results = {}
    summary = [f"### {preset}\n", "| Binary | Status |", "| --- | --- |"]
    details = []
    for binary in TESTS:
        logfile = output / f"{binary}.log"
        if not supported:
            status, code = "not run on this CPU", None
            logfile.write_text(f"{tier} not supported by the container loader on this CPU\n")
        else:
            env = dict(os.environ, ASAN_OPTIONS="detect_leaks=1:allow_addr2line=1")
            with logfile.open("w") as stream:
                try:
                    code = subprocess.run([f"build/{preset}/bin/{binary}", "--gtest_brief=1",
                                           "--gtest_print_time=0"], env=env, stdout=stream,
                                          stderr=subprocess.STDOUT, timeout=3600).returncode
                except subprocess.TimeoutExpired:
                    stream.write("\nTest timed out after 3600 seconds\n")
                    code = 124
            status = "pass" if code == 0 else "fail"
        results[binary] = {"status": status, "exit_code": code}
        summary.append(f"| {binary} | {status} |")
        if status == "fail":
            # Full output is retained in the artifact; bound the job summary.
            log = logfile.read_text(errors="replace")[-12000:]
            details.append(f"\n<details><summary>{binary} failing output (tail)</summary>\n\n"
                           f"````\n{log}\n````\n</details>\n")
    report = {"preset": preset, "commit": run("git", "rev-parse", "HEAD"),
              "subject": run("git", "show", "-s", "--format=%s", "HEAD"),
              "date": run("git", "show", "-s", "--format=%cI", "HEAD"),
              "builder_image": image, "tests": results}
    (output / "result.json").write_text(json.dumps(report, indent=2) + "\n")
    (output / "tests.md").write_text("\n".join(summary) + "\n" + "".join(details))


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def assemble(directory, commit, image, presets):
    """Validate the complete fan-in before touching the previous nightly."""
    directory = Path(directory)
    assets = directory / "release"
    assets.mkdir()
    reports = []
    for preset in presets:
        leg = directory / f"nightly-{preset}"
        report = json.loads((leg / "result.json").read_text())
        if report["commit"] != commit or report["preset"] != preset:
            raise ValueError(f"Wrong source or preset in {leg}")
        if not re.fullmatch(r"sha256:[0-9a-f]{64}", report["builder_image"]):
            raise ValueError(f"Missing immutable builder image in {leg}")
        if set(report["tests"]) != set(TESTS) or any(
                result["status"] not in ("pass", "fail", "not run on this CPU")
                for result in report["tests"].values()):
            raise ValueError(f"Invalid test results in {leg}")
        sources = {p.name: p for p in (leg / "assets").iterdir() if p.name != "SHA256SUMS"}
        checksums = {}
        for line in (leg / "assets/SHA256SUMS").read_text().splitlines():
            checksum, name = line.split("  ", 1)
            if name in checksums:
                raise ValueError(f"Duplicate asset in {leg}")
            checksums[name] = checksum
        if not sources or set(sources) != set(checksums):
            raise ValueError(f"Incomplete assets in {leg}")
        for name, source in sources.items():
            if not re.fullmatch(r"luxir-[A-Za-z0-9_.+-]+", name) or not source.is_file():
                raise ValueError(f"Invalid asset: {source}")
            if digest(source) != checksums[name] or (assets / name).exists():
                raise ValueError(f"Corrupt or duplicate asset: {source}")
            shutil.copyfile(source, assets / name)
        reports.append(report)
    if len({(r['subject'], r['date']) for r in reports}) != 1:
        raise ValueError("Inconsistent commit metadata")
    files = sorted(assets.iterdir())
    (assets / "SHA256SUMS").write_text("".join(f"{digest(p)}  {p.name}\n" for p in files))
    first = reports[0]
    notes = [f"Commit: `{commit}`", f"Subject: {first['subject']}", f"Date: {first['date']}",
             f"Dependency image: `{image}`", "", "Tests are advisory; failures do not block publication.",
             "", "| Preset | luxir_test | decoded_successor_no_avx512_test | decoded_successor_scalar_test | Builder image ID |",
             "| --- | --- | --- | --- | --- |"]
    for report in reports:
        statuses = " | ".join(report["tests"][binary]["status"] for binary in TESTS)
        notes.append(f"| {report['preset']} | {statuses} | `{report['builder_image']}` |")
    notes.append(f"\nFull test output: https://github.com/{os.environ['GITHUB_REPOSITORY']}/actions/runs/{os.environ['GITHUB_RUN_ID']}")
    notes_file = directory / "notes.md"
    notes_file.write_text("\n\n".join(notes[:5]) + "\n" + "\n".join(notes[5:]) + "\n")
    return assets, notes_file


def publish(directory, commit, image, *presets):
    presets = presets or os.environ["NIGHTLY_PRESETS"].split()
    assets, notes = assemble(directory, commit, image, presets)
    stage = f"nightly-staging-{os.environ['GITHUB_RUN_ID']}-{os.environ['GITHUB_RUN_ATTEMPT']}"
    # Upload first. A failed upload leaves nightly and its tag untouched.
    gh("release", "create", stage, "--draft", "--prerelease", "--target", commit,
       "--title", "nightly", "--notes-file", str(notes), *map(str, sorted(assets.iterdir())))
    old = api("GET", "releases/tags/nightly", missing_ok=True)
    if old is not None:
        api("DELETE", f"releases/{old['id']}")
    if api("GET", "git/ref/tags/nightly", missing_ok=True) is None:
        api("POST", "git/refs", {"ref": "refs/tags/nightly", "sha": commit})
    else:
        api("PATCH", "git/refs/tags/nightly", {"sha": commit, "force": True})
    gh("release", "edit", stage, "--tag", "nightly", "--draft=false", "--prerelease", "--latest=false")
    # Draft creation normally creates no tag; clean it if the API did create one.
    if api("GET", f"git/ref/tags/{stage}", missing_ok=True) is not None:
        api("DELETE", f"git/refs/tags/{stage}")


if __name__ == "__main__":
    {"plan": plan, "test": test, "publish": publish}[sys.argv[1]](*sys.argv[2:])
