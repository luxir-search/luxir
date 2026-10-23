# Dates and time zones

Luxir accepts ISO-8601 dates, numeric epoch values, and both Solr and
Elasticsearch/OpenSearch date-math syntax. Date fields support queries,
ranges, sorting, and facets. Use the built-in `_dt` template (`_dts` for
multiple dates), or define a date field explicitly. Partial dates match the
whole day or month they name. Set `time_zone` on a request for local date
queries and calendar buckets. Dates are stored as int64 counts since the Unix
epoch, in milliseconds by default. This page covers the
accepted forms, date math, and time zones; the
[query language](query-language.md#dates-in-expressions) has the grammar
corners and [faceting](faceting.md#date-histograms) has calendar buckets.

## Accepted date values

A date field's `unit` is `"ms"` (default) or `"ns"`, inherited from its parent
when omitted. Numeric ingest and query literals, including integer strings and
date-math anchors, use that unit by default. Numeric facet bounds, gaps, origins
and selected values also default to the field's unit; request flags can select
another input unit. ISO-8601 input converts to the
field's precision. For example:

```json
{"fields":{"event_time":{"type":"date","unit":"ns"}}}
```

Nanosecond ingest accepts values from `INT64_MIN+1` through `INT64_MAX-1`,
covering 1677-09-21 to 2262-04-11; the two extrema are reserved and rejected.
Numeric ms ingest rejects values outside years -32767 through 32767.

Query bounds outside the ns domain saturate to the reserved extrema, so an
upper bound above the domain includes everything and a lower bound above
it includes nothing. The extrema themselves are never stored values.

The unit is part of the field's signature. A schema update cannot change it
on an existing date definition, including an empty field or an inherited unit.

For default millisecond fields, date values and query examples use epoch
milliseconds (not a year) or ISO-8601 strings:

```
2024-06-25T10:30:00Z        a full instant, UTC
2024-06-25T10:30:00-05:00   a full instant with an offset
2024-06-25T10:30            no zone: UTC by default
2024-06-25                  a whole day
2024-06                     a whole month
1719311400000               epoch milliseconds
```

Match and range queries use a date's whole window at its literal precision.
Numeric literals name one field tick. `any_of` instead tests exact instants:
numbers are exact field ticks, ISO text uses its start rounded toward earlier
time, and members outside the field's stored domain are dropped.

A partial date names its whole window at its own granularity. Querying
`when_dt:2024-06` matches every instant in June; the range
`[2024-01 TO 2024-06]` covers January through June, endpoints included.

ISO fractions retain up to nine digits during parsing and date math; further
digits are truncated. Storage truncates to field precision, always toward
earlier time. For example, on a default millisecond field,
`1969-12-31T23:59:59.9999999999Z` stores as epoch millisecond `-1` and finds
that value when used as a query literal.

## Request flags and literal units

The request and each operation wrapper accept a `flags` array with one date
mode. Subops inherit it unless they name a replacement:

| Name | Numeric date literal unit |
|---|---|
| `date_native` (default) | Each field's own unit. |
| `date_iso` | Each field's own unit. |
| `date_ms` | Milliseconds for every date field. |
| `date_ns` | Nanoseconds for every date field. |

These units apply to that operation's query literals in every query dialect,
its domain queries/filters, and facet start/end/gap/origin/selected values.
For example, `date_ms` makes `stamp:2` mean epoch millisecond 2 on an ns field.
Match and range literals cover the input tick; `any_of` members still denote
one instant, floored to storage precision. ISO-8601 text is always accepted
and keeps its textual precision. Ingest is unaffected.

Numeric constants in value expressions and aggregate expressions always use
the date operand's native unit. Date response formatting for these modes is
not enabled yet; output remains numeric in the field's native unit.

Naming `date_native` restores field-native literals under an overridden
parent. `[]` inherits unchanged. Duplicate or conflicting names are errors.
See [JSON request flags](http-api.md#packed-request-flags) or
[protobuf flag words](grpc.md#packed-request-flags).

## Value expressions and aggregates

Date values retain their field unit through `def`, `min`, `max`, `avg`, and
`percentile`. Numeric offsets and `def` fallbacks use that unit; subtracting
two dates returns a numeric duration in their common unit. Combining ms and
ns dates converts the ms operand to ns, and any DATE result uses ns.
Conversion uses multiplication's existing overflow rules: per-document int64
overflow is an evaluation error, while aggregate expressions use wider intermediates.
`sum` rejects dates;
multiplication and division explicitly turn dates into numbers. Date averages
and percentiles keep their double numeric lane, including fractional ticks.

## Date math

Anywhere a date is accepted in a query, date math is too. Both Solr and
Elasticsearch/OpenSearch syntaxes work without configuration. An expression
is an anchor (`NOW` / `now`, or any literal above) followed by add, subtract,
and round commands, evaluated left to right:

| Solr | Elasticsearch/OpenSearch | Meaning in Luxir |
|---|---|---|
| `NOW-30DAYS` | `now-30d` | Thirty days ago. |
| `NOW/DAY` | `now/d` | Today, as a whole-day window. |
| `NOW-1MONTH/MONTH` | `now-1M/M` | All of last month. |
| `2024-06-25T00:00:00Z+6MONTHS` | `2024-06-25T00:00:00Z\|\|+6M` | Six months after the given date. |

Solr syntax appends word units directly to the anchor (`DAYS`, `MONTHS`,
case-insensitive; `WEEKS` is a Luxir extension). Elasticsearch/OpenSearch
syntax uses one-letter units (`d`, `M`, case-sensitive: `M` is month, `m` is
minute), with `||` separating a literal date from its math. Math after `now`
needs no separator. `NOW` and `now` use one clock snapshot for the whole
request, so every clause in a request sees the same instant.

Rounding produces a window, and a range endpoint uses the appropriate edge
of it: `when_dt:>=NOW/DAY` means "from the start of today" and
`when_dt:<=NOW/DAY` means "through the end of today".

Round relative ranges that repeat, such as dashboard filters. `now-1h` moves
with every request millisecond, so a range with an unrounded `NOW` bound is
never cached: it is evaluated in full on every request. `now-1h/m` to `now/m`
is the same filter for a whole minute, so repeated requests reuse the cached
result. Pick the coarsest unit the use case tolerates; the rounded range can
include up to one unit more at each end than the unrounded one.

## Time zones

By default everything above is UTC. Set `time_zone` on a search request to
use another time zone for dates and date math:

```http
POST /collections/main/_search

{"time_zone": "America/Denver",
 "query": {"range": {"field": "when_dt", "gte": "NOW/DAY"}}}
```

(`time_zone` is a request-level setting: it sits beside the query, not inside
it, and applies to every date in the request.)

The zone can be an IANA name (case-sensitive) or a fixed offset such as
`+05:30`. It changes three things:

- Offset-less literals become local civil times: `2024-06-25` is the Denver
  day, not the UTC day.
- Rounding is local: `NOW/DAY` starts at Denver midnight.
- `DAY` and larger units add calendar time: across a daylight-saving change,
  `+1DAY` lands at the same local clock time even when that is 23 or 25
  physical hours away. Hours and smaller stay fixed physical durations.

`NOW`, numeric epoch values, and literals with `Z` or an explicit offset are
instants; the zone does not move them, only math applied to them.

Around a daylight-saving change, local times that do not exist are shifted
forward by the gap, and repeated local times take the earlier occurrence. In
the rare case where a zone change skips an entire calendar date, the request
still succeeds and returns a `date_granule_skipped` warning naming the date.

**Ingest is always UTC.** A document ingested with `"when_dt": "2024-06-25"`
is anchored at UTC midnight even if later queries use a zone. When the same
text must mean the same instant on both paths, write it with `Z` or an
explicit offset.

## Date histograms

Range facets bucket DATE fields with either a fixed gap in field units or a
calendar gap in the facet's zone. Set `"bounds": "data"` to derive the span
from matching values; each nested instance has its own extent. Explicit
`start` and `end` instead set a fixed span:

```http
POST /collections/main/_search

{
  "time_zone": "America/Denver",
  "query": {
    "range": {
      "field": "when_dt",
      "gte": "NOW/DAY-7DAYS"
    }
  },
  "ops": {
    "per_day": {
      "range_facet": {
        "field": "when_dt",
        "start": "NOW/DAY-7DAYS",
        "end": "NOW/DAY+1DAY",
        "calendar_gap": {
          "n": 1,
          "unit": "day"
        }
      }
    }
  }
}
```

Buckets are returned in order, zero counts included, with `[start, end)`
pairs in the field's unit as bucket ids. Nanosecond fields support one-ns
fixed gaps, including negative epoch values; DATA and EXPLICIT use the same
grid. An ns bucket's exclusive end may be `INT64_MAX`. Calendar buckets follow
the civil calendar: months vary in length, and a daylight-saving day is 23 or 25 hours
wide. Fixed gaps also step in local time when a zone is set: repeated lattice
times have both occurrences as fences, and skipped lattice times map to the
transition instant. With EXPLICIT bounds, the phase comes from the local time
of `start`. A facet may set its own `time_zone`; leaving it empty inherits the
request's. See [range facets](faceting.md#range-facets) for DATA widening,
`origin`, calendar alignment, DST behavior and limits.
