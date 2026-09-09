# Faceting and metrics

Facets are search operations over a document domain. Put them under
`top_docs.ops` and they see every document matched by that query and its
filters, not merely the hits returned by `limit`. That lets one request return
the documents for a page and the analytics used to navigate it.

```http
POST /collections/books/_search

{
  "ops": {
    "results": {
      "top_docs": {
        "query": "title_t:dune",
        "filter": ["stock_i:>0"],
        "limit": 10,
        "get_number": true,
        "fields": ["id","title_t"],
        "ops": {
          "categories": {
            "field_facet": {"field":"category_s","limit":10,"missing":true}
          }
        }
      }
    }
  }
}
```

```json
{
  "found": 42,
  "docs": [{"id":"b1","title_t":"Dune"}],
  "ops": {
    "categories": {
      "buckets": [
        {"val":"science-fiction","count":31},
        {"val":"classic","count":18}
      ],
      "missing": 2
    }
  }
}
```

Counts are exact. A multi-valued field contributes its document to each value
it contains, but never more than once to the same bucket.

## Field facets

A `field_facet` groups by distinct field value:

```json
{
  "field_facet": {
    "field": "category_s",
    "limit": 10,
    "mincount": 1,
    "missing": true
  }
}
```

The controls are:

| Field | Meaning |
|---|---|
| `field` | Field to group. Required. |
| `limit` | Maximum returned buckets. Default `5`; `-1` returns all. |
| `mincount` | Drop buckets below this domain count. |
| `missing` | Return the count of documents with no accepted value. |
| `sorts` | Sort a string/ID facet by one named metric sub-operation. |
| `ops` | Per-bucket sub-facets, numeric metrics, or `top_docs` / `fusion` result lists on string/ID facets. |
| `selected` | Values that refine the result set and remain visible as buckets. |
| `selection_mode` | Match any selected value (default) or require all of them. |

String and ID facets default to count descending, then bucket value ascending
as a deterministic tie break. Setting `mincount: 0` can include values that
exist in the collection but have zero matches in the current domain.

Integer and date fields facet on each distinct column value. Text fields facet
on analyzed terms, not on the original stored text: faceting `body_t` answers
"which indexed terms occur?", while faceting `category_s` answers "which
category values occur?" Integer/date/text field facets currently support
`limit`, positive `mincount`, and `missing`, but not sub-operations or custom
sorts. Use a range facet when numeric values should be bucketed rather than
enumerated.

A nonempty `selected` is supported for facets directly under `top_docs.ops`.
The selected values refine the document result; `selection_mode: "all"`
requires every value instead of the default any-value match. The field facet's
ordinary page is still finalized over all buckets. A selected value that lands
in that page appears once at its natural sorted position. After the page,
selected values not already emitted append in request order with exact counts.
These appended buckets are exempt from `mincount`, may name values absent from
the index, and carry the same sub-operation results as ordinary buckets.

## Expression metrics

An expression metric folds document values over its incoming domain. `avg`,
`sum`, `min`, and `max` are the bucket aggregates. The shortest JSON form is a
bare expression string in an operation position:

```json
"average_price": "avg(price_f)"
```

The equivalent explicit forms are
`{"expr_op":"avg(price_f)"}` and
`{"expr_op":{"expr":"avg(price_f)"}}`. The object form also accepts a
`vars` map for scalar `$name` values.

Aggregate arguments use the same numeric value-expression language as sort and
rescore expressions, including arithmetic, parentheses, `def`, unary math, and
explicit per-document array reducers. This makes composite metrics direct:

```json
"weighted_price": "sum(price_f * qty_i) / sum(qty_i)"
```

A bare multi-valued column is not implicitly pooled. For example,
`avg(prices_fs)` is rejected; use `avg(avg(prices_fs))` to average each
document's array first and then average those per-document values across the
bucket. Missing document values are skipped by an aggregate. `def(value, 0)`
can opt a missing value back into its denominator or sum.

Integer `sum`, `min`, and `max` results are returned as int64 values; integer
sums accumulate exactly. `avg` and any floating-point expression return a
double. A domain with no contributing values returns `null`. Data-dependent
overflow or a non-finite aggregate also returns `null` and adds a response
warning rather than failing the request.

For DATE values, `avg`, `min`, and `max` are legal and retain date meaning;
`sum(DATE)` is rejected as a unit clash. Multiplication, division, unary minus,
and unary math demote a DATE expression to an ordinary number.

At the request root or directly under `top_docs.ops`, the result is one scalar
over the incoming domain. Nested under a string or ID facet, it is evaluated
independently for each bucket:

```json
"categories": {
  "field_facet": {
    "field": "category_s",
    "limit": 10,
    "ops": {
      "average_price": {
        "expr_op": "avg(price_f)"
      },
      "lowest_price": "min(price_f)"
    }
  }
}
```

The HTTP response keeps each metric beside its bucket:

```json
{
  "buckets": [
    {"val":"paperback","count":20,"average_price":11.25,"lowest_price":5.99},
    {"val":"hardcover","count":8,"average_price":24.50,"lowest_price":15.00}
  ]
}
```

Sort a string/ID facet by one of its metric operations:

```json
{
  "field_facet": {
    "field": "category_s",
    "limit": 5,
    "ops": {
      "average_price": "avg(price_f)"
    },
    "sorts": [{"expr":"average_price","dir":"desc"}]
  }
}
```

Facet sort expressions are resolved contextually as metric operation names;
document value expressions are not evaluated for buckets. Both `asc` and
`desc` are supported, and a missing or failed metric sorts last in either
direction. Only one facet sort key is supported. Custom count and bucket-value
sort specifications are not yet supported; omit `sorts` for the default count
order.

Facet aggregate state is bounded by the server's
`search.request-memory-max-bytes` per-request query-memory ceiling. The engine
charges the resolved state stride for every simultaneously resident bucket. A
metric needed to sort candidates cannot be deferred; if its estimated state
would exceed the ceiling, the request-memory breaker reports its attempted
total and ceiling together with the facet and metric names.

## Nested facets

String and ID facets can contain other string/ID facets. Each sub-facet sees
only the documents in its parent bucket:

```json
"category": {
  "field_facet": {
    "field": "category_s",
    "limit": 10,
    "ops": {
      "publisher": {
        "field_facet": {"field":"publisher_s","limit":5}
      }
    }
  }
}
```

```json
{
  "buckets": [
    {
      "val": "science-fiction",
      "count": 31,
      "publisher": {
        "buckets": [
          {"val":"ace","count":12},
          {"val":"orb","count":7}
        ]
      }
    }
  ]
}
```

This is one tree, not a follow-up query per bucket. Sub-facets and metrics run
over the bucket domains as part of the original request.

## Top documents per bucket

A `top_docs` (or `fusion`) operation under a facet returns a ranked list of
documents for every bucket. It is an ordinary operation: a `top_docs` applies
its own `query`, `filter`, `sorts`, `limit`, `fields`, and `get_number` to
that bucket's documents, and a `fusion` fuses its sources over them. A
`top_docs` without a `query` selects every document in the bucket, and without
`sorts` such a list is in index order, so give it the query whose ranking you
want (a text query's scores do not depend on the bucket, so repeating the
outer query ranks each bucket's documents the way the main result list does)
or a sort:

```json
"category": {
  "field_facet": {
    "field": "category_s",
    "limit": 10,
    "ops": {
      "best": {
        "top_docs": {
          "query": "title_t:dune",
          "limit": 2,
          "fields": ["id","title_t"],
          "get_number": true
        }
      }
    }
  }
}
```

```json
{
  "buckets": [
    {
      "val": "science-fiction",
      "count": 31,
      "best": {"found": 31, "docs": [{"id":"b1","title_t":"Dune"}, {"id":"b7","title_t":"Dune Messiah"}]}
    },
    {
      "val": "classic",
      "count": 18,
      "best": {"found": 18, "docs": [{"id":"b3","title_t":"Dune"}, {"id":"b9","title_t":"Children of Dune"}]}
    }
  ]
}
```

Per-bucket lists work under string/ID, range, and query facets. A per-bucket
`top_docs` may carry its own `ops`, which see every document in the bucket
that matches that `top_docs` (its query and filters), regardless of its
`limit`; `fusion` accepts no `ops`. Two differences from a top-level list: it
is never streamed in batches, so `batch_size` is ignored and every requested
row arrives in the final response; and a `document_format` left at the default
follows the transport default rather than the enclosing operation's format.
Each bucket is ranked independently, which costs one pass over the bucket's
documents per bucket.

## Range facets

A `range_facet` builds ordered, half-open `[start,end)` buckets over an int,
float, double, or date field:

```json
"price_ranges": {
  "range_facet": {
    "field": "price_i",
    "start": 0,
    "end": 30,
    "gap": 10,
    "mincount": 0,
    "missing": true
  }
}
```

```json
{
  "buckets": [
    {"val":[0,10],"count":4},
    {"val":[10,20],"count":9},
    {"val":[20,30],"count":0}
  ],
  "missing": 2
}
```

`start` and `end` are required, and exactly one of `gap` or `calendar_gap`
must be present. The final bucket is shortened if the gap does not divide the
range evenly. At most 100,000 buckets may be requested.

Every in-range bucket is returned in fence order by default, including zero
counts. `mincount: 0` retains all of them; a positive value filters after
counting. NaN and infinity values fall outside every floating-point bucket, and
a gap too fine for the field's representation at that magnitude is rejected.

Range facets accept sub-operations. Each retained range bucket supplies its
document domain to the child operation, so an expression metric or nested
string facet has the same semantics as it does under a string bucket. Empty
range buckets therefore emit null expression metrics. The executor processes
stateful child bindings in bounded blocks rather than retaining every bucket's
state at once. Range facets with sub-operations are limited to 1,024 buckets.
Custom sorts are not yet supported for range facets; buckets remain in fence
order.

Range facet `selected` values are generated bucket lower fences. They refine
the result in the same way and remain in fence order; selection only exempts a
range bucket from `mincount` admission. This is the same union rule as field
facets, whose natural order is their requested sort rather than fence order.

The HTTP renderer currently emits `null` for float/double range bucket bounds
even though counts and the typed gRPC bounds are correct; integer and date
bounds render normally over HTTP.

For numeric/date fields declared with `index: "range"`, a points index can
answer a whole-index range facet from indexed points instead of walking the
column. The result contract is the same without the index.

## Date histograms

Date bounds accept epoch milliseconds, ISO-8601, and date math. Use
`calendar_gap` when buckets should follow civil days, weeks, months, quarters,
or years:

```json
"by_month": {
  "range_facet": {
    "field": "published_dt",
    "start": "NOW/MONTH-12MONTHS",
    "end": "NOW/MONTH+1MONTH",
    "calendar_gap": {"n":1,"unit":"month"},
    "time_zone": "America/New_York"
  }
}
```

The facet `time_zone` controls offset-less bounds and calendar stepping. Empty
inherits the request time zone; `UTC` explicitly overrides it. IANA zone names
and fixed offsets are accepted. Bucket IDs remain raw epoch-millisecond fence
pairs, because they identify instants unambiguously.

Calendar fences are derived from the original start rather than repeatedly
adding to the previous fence. A sequence starting on January 31 therefore
produces February 28 or 29 and then March 31 instead of drifting. Days across a
daylight-saving change can be 23 or 25 physical hours. Week buckets begin on
Monday.

For a fixed UTC epoch-day number in the value-expression language, use
`floor(when_dt / 86400000)`. Use a date range facet with `calendar_gap` instead
when day bucketing must follow a civil time zone and daylight-saving changes.

Nonexistent civil fences shift forward through the zone transition. If a zone
change removes a whole nominal bucket, Luxir removes the zero-width bucket and
returns a `calendar_bucket_skipped` warning rather than silently changing the
calendar. See [Dates and Time Zones](dates.md) for the date-math and civil-time
contract.

## Query facets

A `query_facet` names an ordered set of arbitrary query buckets. In JSON,
`buckets` is an object: each object key is the bucket name and each value is a
query. A bare string uses the `expr` query syntax; structured queries use their
ordinary object form.

```json
"price_tiers": {
  "query_facet": {
    "buckets": {
      "budget": "price_i:[* TO 100]",
      "mid": {"range":{"field":"price_i","gte":100,"lt":1000}},
      "nearby": {"match":{"field":"region_s","val":"local"}}
    },
    "ops": {
      "average_price": "avg(price_i)"
    }
  }
}
```

The response keeps request order and retains every requested bucket, including
zero counts:

```json
{
  "buckets": [
    {"val":"budget","count":12,"average_price":72.5},
    {"val":"mid","count":31,"average_price":340.0},
    {"val":"nearby","count":0,"average_price":null}
  ]
}
```

Query buckets count documents, not value occurrences. A document contributes
at most once to a query bucket even when several field values make its query
match. Field and range facets instead count matching field values, so one
multi-valued document can contribute to several value-derived counts.

Any query kind can define a bucket, including boolean, range, expression, and
kNN queries. A kNN bucket prepares against the facet's complete incoming
domain, so its `k` nearest documents are chosen from the same filtered domain
the facet sees. Query facets have no bucket-count limit; request size is the
natural bound.

The `ops` map runs independently over each bucket's document domain. It accepts
the same expression metrics, nested facets, and per-bucket `top_docs` as other
fixed-bucket facets.

For multi-select navigation, put a nonempty `selected` on a query facet
directly under `top_docs.ops`. Values are bucket names:

```json
"price_tiers": {
  "query_facet": {
    "buckets": {
      "budget": "price_i:[* TO 100]",
      "mid": "price_i:[100 TO 1000]"
    },
    "selected": ["budget","mid"],
    "selection_mode": "any"
  }
}
```

The default `any` mode refines the result list to the union while computing
this facet sideways, without its own derived filter. Sibling operations see
the filter. `selection_mode: "all"` requires every selected bucket and uses
the resulting strict incoming domain for this facet as well.
