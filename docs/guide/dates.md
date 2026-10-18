# Dates and time zones

Luxir accepts ISO-8601 dates, epoch milliseconds, and both Solr and
Elasticsearch/OpenSearch date-math syntax. Date fields support queries,
ranges, sorting, and facets. Use the built-in `_dt` template (`_dts` for
multiple dates), or define a date field explicitly. Partial dates match the
whole day or month they name. Set `time_zone` on a request for local date
queries and calendar buckets. Dates
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

## Range index for timestamps

A DATE field is always stored as a column. `index: "range"` also builds a
points index, a sorted (value, doc) copy of the same values. Without it,
ranges and counts use the column's per-block and per-leaf min/max zones:
blocks wholly inside or outside the range are counted or skipped from those
zones, and only the few values near each range bound are decoded. That works
when documents arrive in roughly time order, as event and log timestamps do,
because each block then spans a short slice of time.

Measured on 4M events over 30 days in one segment (gcc-release, median
per-segment latency; counts are hit totals, filters iterate every match):

| Arrival order | Column bytes/value | Points bytes/value | Count, column only | Count, with points | 1 h filter, column only | 1 h filter, with points | 7 d filter, column only | 7 d filter, with points |
|---|---|---|---|---|---|---|---|---|
| time-sorted | 1.98 | 2.48 | 6 us | 0.3 us | 61 us | 7 us | 2.4 ms | 2.2 ms |
| 1 h jitter | 3.03 | 4.23 | 29-40 us | 0.3 us | 112 us | 39 us | 2.5 ms | 2.7 ms |
| random | 4.07 | 5.23 | 4.4-4.9 ms | 0.3 us | 14 ms | 39 us | 20 ms | 3.3 ms |

Windows of a day or more filter at about the same speed either way on
time-ordered data, since collecting the matches dominates. Omit
`index: "range"` on a timestamp whose documents arrive in time order: the
points index more than doubles the field's storage to save a few
microseconds per segment on counts and tens of microseconds on narrow
filters. Keep it for dates that arrive in no particular order (birth dates,
due dates, backfilled data), where the zones prune nothing and every query
decodes the whole column.
