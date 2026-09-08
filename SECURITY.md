# Security Policy

## Reporting a vulnerability

Email security@luxir.org. Do not open a public GitHub issue for a security
problem.

Include the version or commit, a description of the issue, and steps or a
request that reproduces it. You will get an acknowledgement within a few
days and a follow-up once the report has been assessed. Please allow time
for a fix before disclosing publicly; we will coordinate a disclosure date
with you.

## Supported versions

Luxir is pre-release. Fixes land on `main` and in the next tagged release;
there are no maintenance branches for earlier versions.

## Scope

Luxir is a single-node engine with no built-in authentication or TLS. It is
designed to run behind your own network and security boundary, as described in
the [operations guide](docs/guide/operations.md). Reports that a server
reachable from an untrusted network can be queried or modified are therefore
out of scope.

In scope, whether or not the server is exposed:

- Memory-safety bugs reachable through the HTTP or gRPC APIs or through
  indexed documents (crashes, out-of-bounds reads, use-after-free).
- Requests that cause unbounded resource consumption disproportionate to the
  request itself.
- Reads or writes outside the configured data directory.
- Anything in the build or dependency setup that could execute or fetch
  untrusted code.
