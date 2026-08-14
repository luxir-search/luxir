# Dates and time zones

Luxir stores dates as int64 milliseconds since the Unix epoch. Name a field
with the `_dt` suffix (or `_dts` for multi-valued) and it is a DATE field you
can query, range, sort, and facet on. This page covers the accepted date
forms, date math, and how time zones change what a request means. The full
grammar lives in [query-language.md](query-language.md); date histograms are
covered in [faceting.md](faceting.md).

## Accepted date values

At ingest and in queries, a DATE value is either a number (epoch
milliseconds, never a bare year) or an ISO-8601 string:

```
2024-06-25T10:30:00Z        a full instant, UTC
2024-06-25T10:30:00-05:00   a full instant with an offset
2024-06-25T10:30            no zone: UTC by default
2024-06-25                  a whole day
2024-06                     a whole month
1719311400000               epoch milliseconds
```

A partial date names its whole window at its own granularity. Querying
`when_dt:2024-06` matches every instant in June; the range
`[2024-01 TO 2024-06]` covers January through June, endpoints included.

## Date math

Anywhere a date is accepted in a query, date math is too. An expression is an
anchor (`NOW`, or any literal above) followed by add, subtract, and round
commands, evaluated left to right:

```
NOW-30DAYS              thirty days ago
NOW/DAY                 today, as a whole-day window
NOW-1MONTH/MONTH        all of last month
2024-06-25||+2d/d       OpenSearch style: anchor, then math after ||
2024-06-25T00:00:00Z+6MONTHS   Solr style: math appended directly
```

Both Solr word units (`DAYS`, `MONTHS`, case-insensitive; `WEEKS` is a Luxir
extension) and OpenSearch one-letter units (`d`, `M`, case-sensitive: `M` is
month, `m` is minute) work. `NOW` is one clock snapshot for the whole
request, so every clause in a request sees the same instant.

Rounding produces a window, and range endpoints use the edge that makes
sense: `when_dt:>=NOW/DAY` means "from the start of today" and
`when_dt:<=NOW/DAY` means "through the end of today".

## Time zones

By default everything above is UTC. Set `time_zone` on a search request to
give it a civil frame:

```
POST /collections/main/_search
{"time_zone": "America/Denver",
 "query": {"range": {"field": "when_dt", "gte": "NOW/DAY"}}}
```

(`time_zone` is a request-level setting; it mixes directly with query keys in
the shorthand shown here, and sits beside `ops` in the full request form.)

The zone can be an IANA name (case-sensitive) or a fixed offset such as
`+05:30`. It changes three things:

- Offset-less literals become local civil times: `2024-06-25` is the Denver
  day, not the UTC day.
- Rounding is local: `NOW/DAY` starts at Denver midnight.
- `DAY` and larger units add calendar time: across a daylight-saving change,
  `+1DAY` lands at the same local clock time even when that is 23 or 25
  physical hours away. Hours and smaller stay fixed physical durations.

`NOW`, epoch milliseconds, and literals with `Z` or an explicit offset are
instants; the zone does not move them, only math applied to them.

Around a daylight-saving change, local times that do not exist are shifted
forward by the gap, and repeated local times take the earlier occurrence -
the same behavior as OpenSearch. In the rare case where a zone change skips
an entire calendar date, the request still succeeds and returns a
`date_granule_skipped` warning naming the date.

**Ingest is always UTC.** A document ingested with `"when_dt": "2024-06-25"`
is anchored at UTC midnight even if later queries use a zone. When the same
text must mean the same instant on both paths, write it with `Z` or an
explicit offset.

## Date histograms

Range facets bucket DATE fields with either a fixed gap in milliseconds or a
calendar gap in the facet's zone:

```
POST /collections/main/_search
{"time_zone": "America/Denver",
 "ops": {"per_day": {"range_facet": {
   "field": "when_dt",
   "start": "NOW/DAY-7DAYS", "end": "NOW/DAY+1DAY",
   "calendar_gap": {"n": 1, "unit": "day"}}}}}
```

Buckets are returned in order, zero counts included, with `[start, end)`
epoch-millisecond pairs as bucket ids. Calendar buckets follow the civil
calendar: months vary in length, and a daylight-saving day is 23 or 25 hours
wide. A facet may set its own `time_zone`; leaving it empty inherits the
request's. See [faceting.md](faceting.md) for the full contract.
