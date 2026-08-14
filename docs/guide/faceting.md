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
        "query": "title_w:dune",
        "filter": [{"name":"available","query":"stock_i:>0"}],
        "limit": 10,
        "get_number": true,
        "fields": ["id","title_w"],
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
  "docs": [{"id":"b1","title_w":"dune"}],
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
| `ops` | Per-bucket sub-facets or numeric metrics on string/ID facets. |

String and ID facets default to count descending, then bucket value ascending
as a deterministic tie break. Setting `mincount: 0` can include values that
exist in the collection but have zero matches in the current domain.

Integer and date fields facet on each distinct column value. Text fields facet
on analyzed terms, not on the original stored text: faceting `body_w` answers
"which indexed terms occur?", while faceting `category_s` answers "which
category values occur?" Integer/date/text field facets currently support
`limit`, positive `mincount`, and `missing`, but not sub-operations or custom
sorts. Use a range facet when numeric values should be bucketed rather than
enumerated.

## Numeric metrics

`avg`, `sum`, `min`, and `max` are generic operations over a numeric column. They
ignore missing values and return `null` when no value exists in the domain:

```json
"average_price": {
  "gen_op": {"name":"avg","args":["price_f"]}
}
```

`sum` adds every value occurrence, including every element of a multi-valued
field. Integer sums are accumulated exactly and then returned as a `double`, so
values beyond 2^53 may be rounded in the response.

At the root of `top_docs.ops`, the result is one scalar over the entire match
domain. Nested under a string or ID facet, it is evaluated independently for
each returned bucket:

```json
"categories": {
  "field_facet": {
    "field": "category_s",
    "limit": 10,
    "ops": {
      "average_price": {
        "gen_op": {"name":"avg","args":["price_f"]}
      },
      "lowest_price": {
        "gen_op": {"name":"min","args":["price_f"]}
      }
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
      "average_price": {"gen_op":{"name":"avg","args":["price_f"]}}
    },
    "sorts": [{"expr":"average_price","dir":"desc"}]
  }
}
```

Facet sort expressions are resolved contextually as metric operation names;
document value expressions are not evaluated for buckets. Only one facet sort
key is supported. Custom count and bucket-value sort specifications are not yet
supported; omit `sorts` for the default count order.

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

Range-facet sub-operations and custom sorts are not implemented. The HTTP
renderer currently emits `null` for float/double range bucket bounds even
though counts and the typed gRPC bounds are correct; integer and date bounds
render normally over HTTP.

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

Nonexistent civil fences shift forward through the zone transition. If a zone
change removes a whole nominal bucket, Luxir removes the zero-width bucket and
returns a `calendar_bucket_skipped` warning rather than silently changing the
calendar. See [Dates and Time Zones](dates.md) for the date-math and civil-time
contract.
