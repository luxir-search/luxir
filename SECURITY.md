# Security Policy

## Reporting a vulnerability

Please do not report security problems through public GitHub issues,
discussions, or chat. Email [security@luxir.org](mailto:security@luxir.org)
with a description of the problem, the commit or version affected, a request
or sequence of requests that reproduces it, and what you believe the impact
is. You will get an acknowledgement within a few days, and we will keep you
informed while we work on a fix.

We ask that you give us reasonable time to fix the problem before publishing
it. We credit reporters in the fix announcement unless you prefer otherwise.

## Scope

Luxir is pre-1.0 and, by design, currently has no built-in authentication or
TLS. It runs inside a network boundary you provide, and clients on that
network are trusted applications, typically a web tier sending known request
shapes; the [operations guide](docs/guide/operations.md) explains the
deployment model. Reports that Luxir lacks authentication or TLS are known
limitations, not vulnerabilities.

Because clients are trusted, anything a client can do through the API by
design is out of scope: creating any number of collections, sending expensive
queries, or ingesting at a rate that starves other traffic. Limiting what
reaches the engine is the job of the tier in front of it.

In scope is the engine failing to honor its own contracts on input it accepts:

- crashes or memory-safety errors triggered by malformed or oversized
  requests, which the parsers are meant to reject cleanly (they enforce depth
  and size limits for this reason);
- reading or writing files outside the configured data directory, for
  example through a crafted collection or field name;
- corruption or loss of committed data, or a crash that leaves a
  half-published commit visible.

## Supported versions

Security fixes land on `main` first and ship in the next release. Before 1.0,
the most recent release is the only supported one: there are no maintained
older release lines and no backports. Please report against the release you
are running or the `main` commit you built.
