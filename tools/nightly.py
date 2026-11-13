#!/usr/bin/env python3
# Copyright 2020-2026 Yonik Seeley and Luxir contributors
# SPDX-License-Identifier: Apache-2.0
"""Nightly planning, advisory tests, and release assembly. No builds in publish.

Usage: nightly.py plan
       nightly.py test PRESET OUTPUT IMAGE_ID
       nightly.py publish DOWNLOAD_DIRECTORY COMMIT IMAGE LEG [LEG ...]
Legs use platform/preset, e.g. linux-arm64/container-release.
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


def configured_legs():
    return os.environ["NIGHTLY_LEGS"].split()


def split_leg(leg):
    arch, preset = leg.split("/", 1)
    allowed = {
        "linux-amd64": ("container-release", "container-release-v3", "container-release-v4",
                        "container-debug", "container-asan"),
        "linux-arm64": ("container-release",),
        "macos-arm64": ("macos-release",),
    }
    if preset not in allowed.get(arch, ()):
        raise ValueError(f"Invalid nightly leg: {leg}")
    return arch, preset


def published(reference):
    """Whether a public ghcr.io image or artifact tag exists; needs only curl."""
    repository, tag = reference.removeprefix("ghcr.io/").rsplit(":", 1)
    token = json.loads(run("curl", "-fsS", "--max-time", "60",
                           f"https://ghcr.io/token?scope=repository:{repository}:pull"))["token"]
    accept = "application/vnd.oci.image.manifest.v1+json, application/vnd.oci.image.index.v1+json, " \
             "application/vnd.docker.distribution.manifest.v2+json, application/vnd.docker.distribution.manifest.list.v2+json"
    return subprocess.run(["curl", "-fsS", "--max-time", "60", "-o", os.devnull, "-I",
                           "-H", f"Authorization: Bearer {token}", "-H", f"Accept: {accept}",
                           f"https://ghcr.io/v2/{repository}/manifests/{tag}"],
                          stderr=subprocess.DEVNULL).returncode == 0


def plan():
    commit = run("git", "rev-parse", "HEAD")
    tags = dict((line.split()[1], line.split()[0]) for line in
                run("git", "ls-remote", "origin", "refs/tags/nightly", "refs/tags/nightly^{}").splitlines())
    previous = tags.get("refs/tags/nightly^{}", tags.get("refs/tags/nightly"))
    build = os.environ["GITHUB_EVENT_NAME"] == "workflow_dispatch" or previous != commit
    outputs = {"commit": commit, "build": str(build).lower()}
    if build:
        key = run("python3", "tools/benchmarks/deps-key.py", ".")
        macos_key = None
        image = f"ghcr.io/{os.environ['GITHUB_REPOSITORY_OWNER'].lower()}/luxir-dev:{key}"
        matrix = []
        for leg in configured_legs():
            arch, preset = split_leg(leg)
            if arch == "macos-arm64":
                if macos_key is None:
                    macos_key = run("python3", "tools/benchmarks/deps-key.py", ".", "macos")
                tagged = image.rsplit(":", 1)[0] + f":{macos_key}-macos-arm64"
            else:
                tagged = f"{image}-{arch.removeprefix('linux-')}"
            if not any(item["platform"] == arch for item in matrix):
                if not published(tagged):
                    raise RuntimeError(f"Missing {tagged}. Publish reviewed dependency inputs with deps-image.yml first.")
            matrix.append({"platform": arch, "preset": preset, "dependency": tagged,
                           "runner": {"linux-arm64": "ubuntu-24.04-arm", "linux-amd64": "ubuntu-24.04",
                                      "macos-arm64": "macos-15"}[arch]})
        outputs["image"] = ", ".join(sorted({item["dependency"] for item in matrix}))
        outputs["matrix"] = json.dumps({"include": matrix})
    with open(os.environ["GITHUB_OUTPUT"], "a") as stream:
        stream.writelines(f"{key}={value}\n" for key, value in outputs.items())
    print(f"Source: {commit}; " + ("build requested" if build else "nightly already current"))


def failures(log):
    """The failed assertions and test names; the server log between them is noise.

    Full output is retained in the artifact; this bounds the job summary.
    """
    found = re.findall(r"^.*: Failure\n(?:.*\n){0,8}?(?=\n|[IWE] \d\d:)|^\[  FAILED  \] .*\n", log, re.M)
    # A crash or a timeout reports no gtest failure, so show where it stopped.
    return "".join(dict.fromkeys(found))[:12000] or log[-12000:]


def test(preset, output, builder):
    from release_support import architecture, cpu_supported

    output = Path(output)
    cache = (Path("build") / preset / "CMakeCache.txt").read_text()
    tier = re.search(r"^LUXIR_CPU_TARGET:[^=]+=(.+)$", cache, re.M)[1]
    supported = cpu_supported(tier)
    results = {}
    platform = architecture(tier).platform + "-" + architecture(tier).name
    summary = [f"### {platform}/{preset}\n", "| Binary | Status |", "| --- | --- |"]
    details = []
    for binary in TESTS:
        logfile = output / f"{binary}.log"
        if not supported:
            status, code = "not run on this CPU", None
            logfile.write_text(f"{tier} not supported by the container loader on this CPU\n")
        else:
            env = dict(os.environ)
            if architecture(tier).platform == "linux":
                env["ASAN_OPTIONS"] = "detect_leaks=1:allow_addr2line=1"
            else:
                env.pop("ASAN_OPTIONS", None)
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
            details.append(f"\n<details><summary>{binary} failures</summary>\n\n"
                           f"````\n{failures(logfile.read_text(errors='replace'))}\n````\n</details>\n")
    report = {"platform": platform, "preset": preset, "commit": run("git", "rev-parse", "HEAD"),
              "subject": run("git", "show", "-s", "--format=%s", "HEAD"),
              "date": run("git", "show", "-s", "--format=%cI", "HEAD"),
              "builder": json.loads(builder), "tests": results}
    (output / "result.json").write_text(json.dumps(report, indent=2) + "\n")
    (output / "tests.md").write_text("\n".join(summary) + "\n" + "".join(details))


def digest(path):
    with path.open("rb") as stream:
        digest = hashlib.sha256()
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
        return digest.hexdigest()


def validate_builder(builder, platform):
    kind = "native" if platform == "macos-arm64" else "container"
    digest = builder.get("dependency_digest" if kind == "native" else "image", "")
    if builder.get("kind") != kind or not re.fullmatch(r"sha256:[0-9a-f]{64}", digest):
        raise ValueError("Missing immutable builder content")
    if kind == "native" and not builder.get("toolchain"):
        raise ValueError("Missing native toolchain identity")


def assemble(directory, commit, image, legs):
    """Validate the complete fan-in before touching the previous nightly."""
    directory = Path(directory)
    assets = directory / "release"
    assets.mkdir()
    reports = []
    for leg_id in legs:
        arch, preset = split_leg(leg_id)
        leg = directory / f"nightly-{arch}-{preset}"
        report = json.loads((leg / "result.json").read_text())
        if report["commit"] != commit or report["preset"] != preset or report["platform"] != arch:
            raise ValueError(f"Wrong source, platform or preset in {leg}")
        validate_builder(report["builder"], arch)
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
             f"Dependency builders: `{image}`", "", "Tests are advisory; failures do not block publication.",
             "", "| Platform | Preset | luxir_test | decoded_successor_no_avx512_test | decoded_successor_scalar_test | Builder image or dependencies |",
             "| --- | --- | --- | --- | --- | --- |"]
    for report in reports:
        statuses = " | ".join(report["tests"][binary]["status"] for binary in TESTS)
        notes.append(f"| {report['platform']} | {report['preset']} | {statuses} | `{report['builder'].get('image') or report['builder']['dependency_digest']}` |")
    notes.append(f"\nFull test output: https://github.com/{os.environ['GITHUB_REPOSITORY']}/actions/runs/{os.environ['GITHUB_RUN_ID']}")
    notes_file = directory / "notes.md"
    notes_file.write_text("\n\n".join(notes[:5]) + "\n" + "\n".join(notes[5:]) + "\n")
    return assets, notes_file


def publish(directory, commit, image, *legs):
    legs = legs or configured_legs()
    assets, notes = assemble(directory, commit, image, legs)
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
