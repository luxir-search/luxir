# Contributing to Luxir

Thanks for your interest in Luxir. The project is pre-1.0 and moving quickly:
interfaces change without back-compat, and the fastest way to have a change
land is to talk about it before writing much code.

## Questions, ideas, and bugs

- Questions and ideas: [GitHub Discussions](https://github.com/luxir-search/luxir/discussions).
- Bugs and feature requests: [GitHub Issues](https://github.com/luxir-search/luxir/issues).
- Security problems: email <security@luxir.org> as described in
  [SECURITY.md](SECURITY.md), not a public issue.

A good bug report includes the commit or version, the exact request you sent,
the response you got, and what you expected instead. A `curl` reproduction
against a fresh, empty collection is ideal; the
[Quickstart](docs/guide/quickstart.md) shows the shape.

## Making changes

1. For anything beyond a small fix, open an issue or discussion first and say
   what you plan to change. Design questions are cheaper to settle before the
   code exists, and parts of the engine are being reshaped.
2. Build and test locally. [Build setup](docs/dev/build-setup.md) covers the
   toolchain and presets; the [Codebase map](docs/dev/codebase-map.md) explains
   where things live. The default iteration build is `gcc-debug`; run the
   `gcc-debug-asan` build as well before submitting.
3. Add or extend tests for behavior you change. Tests use the helpers in
   `test/test/` (`CollectionHelper.h`, `TestUtils.h`, `LocalReq.h`) rather
   than mocks, and stay short.
4. Follow the conventions in [AGENTS.md](AGENTS.md). That file is written for
   both people and coding agents working in this tree; the rules are the same.
5. Open a pull request against `main` with one logical change per PR.

Commit messages are concise and describe what changed, for example
`facets: exact counts for date histograms with time zones`. They do not
describe process, review rounds, or tooling.

## Use of AI

We require all use of AI in contributions to follow our [AI policy](AI_POLICY.md).
In short: AI tools are fine for writing code, but you remain responsible for
what you submit, you must be able to explain it in your own words, and
communication with maintainers is written by humans, not generated.

## Documentation

User and design documentation lives in [`docs/`](docs/README.md) as plain
GitHub-flavored Markdown and is published to <https://luxir.org> on every
change to `main`. Keep links relative, start each page with a single `#`
heading, and add new pages to the reading order in `docs/README.md`.

## License

Luxir is licensed under the [Apache License 2.0](LICENSE). By contributing,
you agree that your contributions are licensed under the same terms. No
separate contributor agreement is required at this time.

## Conduct

Everyone taking part is expected to follow the
[code of conduct](CODE_OF_CONDUCT.md).
