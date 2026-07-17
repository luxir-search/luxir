# Solux Documentation

New here? Start with the [Quickstart](guide/quickstart.md) - index and search
with `curl` in a few commands.

- [features.md](features.md) - what Solux is and what it can do today: the
  quick-evaluation page.
- [guide/schema.md](guide/schema.md) - field types and templates, analyzers,
  and the `_schema` HTTP API.
- [guide/dates.md](guide/dates.md) - date fields, date math, and time zones:
  what a zoned request means and how to bucket by calendar time.
- [guide/faceting.md](guide/faceting.md) - field and range facets, including
  calendar gaps and facet time zones.
- [guide/](guide/) - user documentation: quickstart, schema, indexing,
  querying, faceting, vector search. Written for someone with a running server
  and a client; no source checkout assumed.
- [design/](design/) - architecture, principles, and design decisions: the
  reference for how the engine works and why it works that way.
- [dev/](dev/) - contributor documentation: building, testing, conventions.
  Assumes a source checkout.

Rule of thumb: if a page only makes sense with the source checked out, it
belongs in dev/ or design/, not guide/.
