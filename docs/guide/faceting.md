# Faceting

Facets are named search operations. A field facet groups matching documents by
stored field value. A range facet groups integer or date values into ordered
half-open ranges.

## Range facets

`RangeFacet` has the following request fields:

```proto
message RangeFacet {
  string field = 1;
  Val start = 2;
  Val end = 3;
  oneof gap_kind {
    Val gap = 4;
    CalendarGap calendar_gap = 10;
  }
  optional int64 mincount = 5;
  bool missing = 6;
  string time_zone = 9;
}

message CalendarGap {
  int32 n = 1;
  enum Unit { UNKNOWN = 0; DAY = 1; WEEK = 2; MONTH = 3; QUARTER = 4; YEAR = 5; }
  Unit unit = 2;
}
```

`start` and `end` are required, and exactly one of `gap` or `calendar_gap`
must be set. The range is `[start, end)`. The last bucket is shortened when
the gap does not divide the range evenly. A facet is rejected if `start` is
not before `end` or if it would contain more than 100,000 buckets.

For an integer field, `start`, `end`, and `gap` are integer values. For a DATE
field, bounds may be epoch milliseconds or any supported date/date-math value,
including `NOW/DAY-30DAYS`. A DATE bound uses the start of the window named by
the expression. A plain DATE `gap` is a fixed number of milliseconds.

Use `calendar_gap` for civil days, weeks, months, quarters, or years. Calendar
fences are derived independently from the original start, so a monthly facet
starting on January 31 produces February 28 or 29 and then March 31 rather
than drifting from the shortened February date. In UTC and fixed-offset zones,
DAY and WEEK gaps have fixed millisecond widths. In IANA zones, their physical
width can change across daylight-saving transitions.

The `time_zone` field controls offset-less DATE bounds and calendar stepping.
An empty value inherits the request's `time_zone`; set `UTC` to override a
non-UTC request explicitly. Fixed offsets such as `+05:30` and case-sensitive
IANA names such as `America/Denver` are also accepted. An explicit offset in a
bound locates that bound's instant, while the facet zone still controls later
calendar fences. Invalid zones are request errors. `time_zone` and
`calendar_gap` are rejected on non-DATE fields.

Nonexistent civil fences are shifted forward by the zone transition. Ambiguous
fences retain the start instant's UTC offset when that offset is valid. If a
zone skipped an entire nominal bucket, Solux removes that zero-width bucket and
returns a `calendar_bucket_skipped` warning. A bound naming a wholly skipped
date granule returns a `date_granule_skipped` warning.

Every in-range bucket is returned in fence order, including buckets with a zero
count. Unset `mincount` and `mincount: 0` both retain all buckets; a positive
value removes buckets below that count, and a negative value is invalid.
Bucket IDs are `[lo, hiExclusive)` pairs. DATE bucket IDs are raw epoch
milliseconds; the effective zone is not repeated in the response.

Date strings used while ingesting documents are still interpreted in UTC.
Request and facet zones affect query-time bounds and calendar construction only.

FLOAT and DOUBLE fields facet the same way with double-valued `start`, `end`,
and `gap` (all finite, `gap > 0`); bucket ids are decoded float/double pairs.
A gap finer than the field type can represent at that magnitude is rejected.
NaN and infinity values fall outside every bucket.

Range-facet sub-operations are not supported yet.
