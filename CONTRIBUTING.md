# Contributing to Luxir

Thanks for your interest. Luxir is pre-release and moving quickly, so this
page is short and will change.

## Terms

Luxir is licensed under the [Apache License 2.0](LICENSE). By submitting a
contribution you agree that it is licensed under the same terms, as described
in section 5 of that license. There is no separate contributor agreement or
sign-off to complete.

## What is welcome now

- Bug reports with a way to reproduce them: the request, the documents that
  were indexed, and the observed versus expected result.
- Small, focused fixes: a wrong result, a crash, a documentation error, a
  build problem on a supported platform.
- Questions and design discussion in GitHub issues.

Before starting anything larger than a focused fix, open an issue describing
the problem and the approach. The API and the on-disk format change without
notice before 1.0, and internal designs are still being reshaped, so a pull
request built on last month's structure can be superseded before it lands.
An issue first avoids wasted work on both sides.

## Building and testing

[Build setup](docs/dev/build-setup.md) covers toolchains and dependencies.
[Codebase map](docs/dev/codebase-map.md) describes where things live, and
[AGENTS.md](AGENTS.md) holds the build, test, and code conventions used in
this repository. In short:

```bash
cmake --preset gcc-debug
cmake --build --preset gcc-debug
./build/gcc-debug/bin/luxir_test --gtest_brief=1 --gtest_print_time=0
```

Before submitting, also run the tests under the `gcc-debug-asan` preset.
Add a test for any behavior change; tests use the helpers in `test/test/` and
never mock. Keep commit messages concise and say what changed.

## Security issues

Do not open a public issue for a security problem. See
[SECURITY.md](SECURITY.md).
