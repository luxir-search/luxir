# Dates and time zones

Date fields support queries, ranges, sorting, and facets. Use the built-in
`_dt` template (`_dts` for multiple dates), or define a date field explicitly.
Dates can be ISO-8601 text, epoch milliseconds, partial dates that match the
whole day or month they name, or date math like `NOW/DAY-30DAYS`. Set
`time_zone` on a request for local date queries and calendar buckets. Dates
are stored as int64 milliseconds since the Unix epoch. This page covers the
accepted forms, date math, and time zones; the
[query language](query-language.md#dates-in-expressions) has the grammar
corners and [faceting](faceting.md#date-histograms) has calendar buckets.

## Accepted date values

At ingest and in queries, a DATE value is either a number (epoch
milliseconds, not a year) or an ISO-8601 string:

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
2024-06-25||+2d/d       one-letter units after a || separator
2024-06-25T00:00:00Z+6MONTHS   Solr style: word units appended directly
```

Both spellings work: Solr word units (`DAYS`, `MONTHS`, case-insensitive;
`WEEKS` is a Luxir extension) appended to the anchor, and one-letter units
(`d`, `M`, case-sensitive: `M` is month, `m` is minute) after `||`. `NOW` is one clock snapshot for the whole
request, so every clause in a request sees the same instant.

Rounding produces a window, and a range endpoint uses the appropriate edge
of it: `when_dt:>=NOW/DAY` means "from the start of today" and
`when_dt:<=NOW/DAY` means "through the end of today".

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

`NOW`, epoch milliseconds, and literals with `Z` or an explicit offset are
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

Range facets bucket DATE fields with either a fixed gap in milliseconds or a
calendar gap in the facet's zone:

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
epoch-millisecond pairs as bucket ids. Calendar buckets follow the civil
calendar: months vary in length, and a daylight-saving day is 23 or 25 hours
wide. A facet may set its own `time_zone`; leaving it empty inherits the
request's. See [faceting.md](faceting.md) for the full contract.
