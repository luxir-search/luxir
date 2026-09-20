# Faceting and metrics

Faceted search groups matching documents into **buckets** and returns
information about each bucket. A book search might group matches by category,
price range, or publication year. Each bucket has a count; you can also compute
metrics such as average price, group its documents into sub-buckets, or retrieve
its top matching documents.
Buckets can overlap: a book can belong to both science-fiction and classics.

In a search UI, these summaries become choices such as "Science fiction (31)"
or "Under $20 (18)". Users can see how results are distributed and select
values to narrow their search.

One request returns the top documents for a page along with the facets and
metrics for it. Put facets and metrics in the **sub operations** `ops` of a
query and they see every document the query and its filters match, not just
the hits returned by `limit`. The ranked documents, the facet counts, and the
total come back in one round trip, over one consistent view of the index.
Counts are exact by default.

For an interactive facet sidebar, start with
[`selected`](#easy-multi-select-with-selected): put the user's choices on each
facet and Luxir builds the filters and handles multi-select counts automatically.

```http
POST /collections/books/_search

{
  "query": "title_t:dune",
  "filter": ["stock_i:>0"],
  "limit": 10,
  "get_number": true,
  "fields": ["id","title_t"],
  "ops": {
    "categories": {
      "field_facet": {"field":"category_s","limit":10,"missing":true}
    },
    "average_price": "avg(price_f)"
  }
}
```

```json
{
  "found": 42,
  "docs": [
    {
      "id": "b1",
      "title_t": "Dune"
    }
  ],
  "ops": {
    "categories": {
      "buckets": [
        {
          "val": "science-fiction",
          "count": 31
        },
        {
          "val": "classic",
          "count": 9
        }
      ],
      "missing": 2
    },
    "average_price": 11.72
  }
}
```

Every entry in `ops` is a named operation, and operations nest. A facet's own
`ops` run once per bucket, so one category facet can return the average
price, a publisher breakdown, and the two best documents of every category.
The whole tree executes as part of the original request rather than as a
follow-up query per bucket.

The operation kinds on this page:

| Operation | Groups by |
|---|---|
| `field_facet` | Each distinct value of a field. |
| `range_facet` | Fixed-width or calendar buckets over a numeric or date field. |
| `query_facet` | Named buckets you define with arbitrary queries. |
| Expression metric (`"avg(price_f)"`) | Nothing; one number over the incoming documents. |
| `top_docs` / `fusion` | Nothing; a ranked document list per bucket. |

## Easy multi-select with `selected`

Use `selected` to send the current choices along with each facet. Luxir turns
them into filters for the results and excludes each facet's own selection
when counting that facet's buckets, so the alternatives stay visible.

Here the user has checked two categories and one format:

```http
POST /collections/books/_search

{
  "query": {"all":true},
  "filter": ["stock_i:>0"],
  "limit": 10,
  "get_number": true,
  "fields": ["id","title_t"],
  "ops": {
    "categories": {
      "field_facet": {
        "field": "category_s",
        "limit": 10,
        "selected": ["science-fiction","fantasy"]
      }
    },
    "formats": {
      "field_facet": {
        "field": "format_s",
        "limit": 10,
        "selected": ["paperback"]
      }
    },
    "average_price": "avg(price_f)"
  }
}
```

The default `selection_mode: "any"` combines choices with **OR within a facet**
and **AND across facets**. Luxir applies the query and the stock filter
everywhere, then handles the selections for each part of the response:

| Result | Selections applied |
|---|---|
| `docs`, `found`, and `average_price` | Science-fiction or fantasy, and paperback. |
| `categories` buckets | Paperback; count all categories so the user can choose more. |
| `formats` buckets | Science-fiction or fantasy; count all formats so the user can choose more. |

Each facet ignores only its own selection when counting its buckets. Update
its `selected` array as the user checks or unchecks values; omit it or send
`[]` to clear that facet's selection. Selected buckets stay in the response
even when outside `limit`, so the UI can keep showing the checked choices.

The same API works for field, range, and query facets directly under the
query's `ops`. See [Multi-select navigation](#multi-select-navigation) for
selection values, normalization, and the stricter `selection_mode: "all"`.

## Field facets

A `field_facet` groups by distinct field value. For example, count books in
each category:

```http
POST /collections/books/_search

{
  "query": {"all": true},
  "limit": 0,
  "ops": {
    "categories": {
      "field_facet": {
        "field": "category_s",
        "limit": 10,
        "missing": true
      }
    }
  }
}
```

The response contains a bucket for each category and its document count:

```json
{
  "docs": [],
  "ops": {
    "categories": {
      "buckets": [
        {
          "val": "science-fiction",
          "count": 31
        },
        {
          "val": "classic",
          "count": 9
        }
      ],
      "missing": 2
    }
  }
}
```

Here, 31 books have category `science-fiction`, 9 have category `classic`,
and 2 have no category. The request's `limit: 0` omits individual books;
the facet's `limit: 10` returns up to ten categories.

| Field | Meaning |
|---|---|
| `field` | Field to group. Required. |
| `limit` | Maximum returned buckets. Default `5`; `-1` returns all. |
| `mincount` | Drop buckets below this count. |
| `missing` | Also return the count of documents with no value in the field. |
| `sort` | Sort a string/ID facet by one of its metric sub-operations: a one-element list such as `["name desc"]` or `["name asc"]`. Default: count descending. |
| `ops` | Per-bucket sub-facets, metrics, or `top_docs` / `fusion` lists on string/ID facets. |
| `selected` | Values that refine the result set and stay visible as buckets. See [Multi-select navigation](#multi-select-navigation). |
| `selection_mode` | `any` (default) or `all`: match any selected value, or require every one. |

String and ID facets return buckets in count-descending order, with bucket
value ascending as a deterministic tie break. A multi-valued field contributes
its document to each value it holds, but never twice to the same bucket.
`mincount: 0` includes values that exist in the collection but have zero
matches in the current result set, which keeps a sidebar stable while the user
narrows a search.

Integer and date fields facet on each distinct column value. Text fields facet
on analyzed terms, not on the stored text: a facet on `body_t` returns
indexed words, while a facet on `category_s` returns whole category values.
Integer, date, and text facets support `limit`, `mincount`, `missing`, and
`selected`, but not `ops` or `sort`; text also accepts `mincount: 0`, while
int and date require a positive value when set. Use a
range facet when numbers should be bucketed rather than enumerated.

A string facet needs indexed terms and a column. Facets return the term as
stored. For fields with multiple representations, see
[Field variants](#field-variants).

## Expression metrics

An expression metric folds document values over its incoming documents. The
shortest form is a bare expression string in an operation position:

```json
"average_price": "avg(price_f)"
```

`avg`, `sum`, `min`, and `max` are the aggregates. Their arguments use the same
value-expression language as [sort expressions](searching.md#sorting):
arithmetic, parentheses, `def`, unary math, and per-document array reducers.
A composite metric is a single expression:

```json
"weighted_price": "sum(price_f * qty_i) / sum(qty_i)"
```

The explicit forms are `{"expr_op":"avg(price_f)"}` and
`{"expr_op":{"expr":"avg(price_f)","vars":{...}}}`; the object form takes a
`vars` map for scalar `$name` values.

Directly under the query's `ops` (or at the root of a full-form request), a
metric is one scalar over the whole match set. Nested under a facet, it is
evaluated independently for each bucket and returned beside the bucket:

```json
"categories": {
  "field_facet": {
    "field": "category_s",
    "limit": 10,
    "ops": {
      "average_price": "avg(price_f)",
      "lowest_price": "min(price_f)"
    }
  }
}
```

```json
{
  "buckets": [
    {
      "val": "paperback",
      "count": 20,
      "average_price": 11.25,
      "lowest_price": 5.99
    },
    {
      "val": "hardcover",
      "count": 8,
      "average_price": 24.50,
      "lowest_price": 15.00
    }
  ]
}
```

Sort a string or ID facet by one of its metrics:

```json
{
  "field_facet": {
    "field": "category_s",
    "limit": 5,
    "ops": {"average_price": "avg(price_f)"},
    "sort": ["average_price desc"]
  }
}
```

Facet `sort` names a metric operation of that facet; `asc` and `desc` are both
supported, and a bucket whose metric is missing or failed sorts last either
way.

The typing rules:

- Column leaves use the field's value binding and require a numeric or date
  column.
- Reduce multi-valued columns within each document first:
  `avg(avg(prices_fs))` averages each document's prices, then averages those
  across the bucket.
- Missing document values are skipped. `def(value, 0)` counts a missing value
  as 0 in a sum or a denominator.
- Integer `sum`, `min`, and `max` return int64 values and accumulate exactly;
  `avg` and any floating-point expression return a double. A bucket with no
  contributing values returns `null`. Data-dependent overflow or a non-finite
  result also returns `null` and adds a response warning rather than failing
  the request.
- `avg`, `min`, and `max` of a date keep their date meaning; `sum` requires
  numeric values. Multiplication, division, unary minus, and unary math turn
  a date into a plain number.

## Nested facets

String and ID facets can contain other string or ID facets. Each sub-facet
sees only the documents in its parent bucket:

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
          {
            "val": "ace",
            "count": 12
          },
          {
            "val": "orb",
            "count": 7
          }
        ]
      }
    }
  ]
}
```

Nesting is not limited to one level, and metrics sit beside sub-facets in the
same `ops` map.

## Top documents per bucket

A `top_docs` (or `fusion`) operation under a facet returns a ranked document
list for every bucket: the best few products per category, the latest post per
author. It is an ordinary operation with its own `query`, `filter`, `sort`,
`limit`, `fields`, and `get_number`, applied to that bucket's documents:

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
      "best": {
        "found": 31,
        "docs": [
          {
            "id": "b1",
            "title_t": "Dune"
          },
          {
            "id": "b7",
            "title_t": "Dune Messiah"
          }
        ]
      }
    },
    {
      "val": "classic",
      "count": 18,
      "best": {
        "found": 18,
        "docs": [
          {
            "id": "b3",
            "title_t": "Dune"
          },
          {
            "id": "b9",
            "title_t": "Children of Dune"
          }
        ]
      }
    }
  ]
}
```

A `top_docs` without a `query`
selects every document in the bucket, and without a `sort` that list is in
index order. A text query's scores do not depend on the bucket, so repeating
the outer query ranks each bucket the way the main result list is ranked.

Per-bucket lists work under string/ID, range, and query facets. A per-bucket
`top_docs` may carry its own `ops`, which see every document in the bucket
that its query and filters match, regardless of its `limit`; a per-bucket
`fusion` accepts no `ops`. Two differences from a top-level list: it is never
streamed in batches (`batch_size` is ignored and every row arrives in the final
response), and a `document_format` left at the default follows the transport
default rather than the enclosing operation's. Each bucket is ranked
independently, which costs one pass over the bucket's documents per bucket.

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
    {
      "val": [0, 10],
      "count": 4
    },
    {
      "val": [10, 20],
      "count": 9
    },
    {
      "val": [20, 30],
      "count": 0
    }
  ],
  "missing": 2
}
```

| Field | Meaning |
|---|---|
| `field` | Numeric or date field. Required; its value binding must be numeric or date. |
| `start`, `end` | Bounds of the bucketed range. Required. |
| `gap` | Fixed bucket width (milliseconds for dates). Exactly one of `gap` or `calendar_gap` is required. |
| `calendar_gap` | `{"n": 1, "unit": "month"}` for civil buckets over dates; see [Date histograms](#date-histograms). |
| `mincount` | `0` (default) keeps every bucket in order, zero counts included; a positive value filters after counting. |
| `missing` | Also return the count of documents with no value. |
| `ops` | Per-bucket metrics, nested facets, or `top_docs`. |
| `selected` | Lower fences of buckets that refine the result set. See [Multi-select navigation](#multi-select-navigation). |
| `time_zone` | Time zone for bounds without an explicit offset and for calendar buckets; empty inherits the request's. |

Every in-range bucket is returned in lower-bound order, including zero counts,
so the result plots directly as a histogram. The final bucket is shortened if the gap does
not divide the range evenly. NaN and infinity fall outside every floating-point
bucket. The gap must be large enough to advance the field's numeric
representation at that magnitude. At most 100,000 buckets may be requested;
range facets with sub-operations are limited to 1,024. Sub-operations have the
same semantics as under a string bucket, so an empty range bucket yields
`null` metrics.

For numeric and date fields declared with `index: "range"`, a points index can
answer a whole-index range facet from indexed points instead of walking the
column. The result is the same either way; only the cost differs.

## Date histograms

Date bounds accept epoch milliseconds, ISO-8601 text, and date math. Use
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

Calendar fences are derived from the original start rather than by repeatedly
adding to the previous fence, so a sequence starting on January 31 produces
February 28 or 29 and then March 31 instead of drifting. Days across a
daylight-saving change are 23 or 25 physical hours. Week buckets begin on
Monday. When a zone change removes a whole nominal bucket, Luxir drops the
zero-width bucket and returns a `calendar_bucket_skipped` warning.
[Dates and time zones](dates.md) has the
date-math and civil-time contract.

For a fixed UTC epoch-day number in the value-expression language, use
`floor(when_dt / 86400000)`; use a calendar gap when day bucketing must follow
a civil time zone.

## Query facets

A `query_facet` names an ordered set of buckets, each defined by a query. In
JSON, `buckets` is an object: each key is the bucket name and each value is a
query, as an expression string or a structured object:

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

```json
{
  "buckets": [
    {
      "val": "budget",
      "count": 12,
      "average_price": 72.5
    },
    {
      "val": "mid",
      "count": 31,
      "average_price": 340.0
    },
    {
      "val": "nearby",
      "count": 0,
      "average_price": null
    }
  ]
}
```

The response keeps request order and retains every requested bucket, including
zero counts. Any query kind can define a bucket: boolean, range, expression,
geo, or kNN. A kNN bucket prepares against the facet's complete incoming
domain, so its `k` nearest documents are chosen from the same filtered set the
facet sees. There is no bucket-count limit beyond request size.

Query buckets count documents, not value occurrences: a document contributes
at most once to a bucket even when several of its values make the query match.
Field and range facets count matching field values instead, so one
multi-valued document can contribute to several value-derived counts.

The `ops` map runs independently over each bucket's documents and accepts the
same metrics, nested facets, and per-bucket `top_docs` as the other facets.

## Multi-select navigation

A faceted UI usually lets a user pick a value and still see the sibling values
they could have picked. Put the chosen values in the facet's `selected` list:
the result set is refined to them, and in the default `any` mode the facet's
own buckets are counted *without* that refinement, so the alternatives stay
visible with their counts. Sibling operations see the refinement.

Using the [author example](documents.md#field-variants), select Neal Asher:

```http
POST /collections/authors/_search

{
  "query": {
    "all": true
  },
  "fields": ["id", "title_t", "author_name"],
  "get_number": true,
  "ops": {
    "authors": {
      "field_facet": {
        "field": "author_name",
        "limit": 10,
        "selected": ["Neal Asher"]
      }
    }
  },
  "sort": "id"
}
```

```json
{
  "found": 3,
  "docs": [
    {
      "id": "b1",
      "title_t": "Gridlinked",
      "author_name": "Neal Asher"
    },
    {
      "id": "b2",
      "title_t": "The Skinner",
      "author_name": "Neal Asher"
    },
    {
      "id": "b4",
      "title_t": "Prador Moon",
      "author_name": "Neal Asher"
    }
  ],
  "ops": {
    "authors": {
      "buckets": [
        {
          "val": "Neal Asher",
          "count": 3
        },
        {
          "val": "Neal Stephenson",
          "count": 2
        },
        {
          "val": "Neal Shusterman",
          "count": 1
        }
      ]
    }
  }
}
```

The document list holds Asher's three books, while the facet still shows all
three authors. Selection uses the facet's resolved representation for both
normalization and refinement, so a returned bucket value can be sent back as
a selection.

`selection_mode: "all"` requires every selected value instead of any of them,
and then the facet's own buckets are counted over that strict set too.

The rules that apply to every facet kind:

- `selected` is supported on facets directly under the query's `ops`: not at
  the root of a full-form request, not under a `fusion`, and not nested inside
  another facet.
- Field facets take values, range facets take bucket lower fences as plain
  numbers (`"selected": [25]` picks the bucket that starts at 25), and query
  facets take bucket names.
- A selected value that lands in the facet's ordinary page appears once, at its
  natural position. Selected values outside the page are appended afterwards
  in request order with exact counts; range facets keep fence order. Appended
  buckets are exempt from `mincount`, may name values absent from the index,
  and carry the same sub-operation results as ordinary buckets.
- Selecting on a text facet uses exact-token membership, with the same
  single-term rule as [`any_of`](query-reference.md#exact-membership-any_of).

For a query facet, values are bucket names:

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

## Field variants

A bare field name uses the field's
[value binding](schema.md#default-bindings). In the
[author example](documents.md#field-variants), the `_name` template sets
`defaults.value` to `s`, so a facet on `author_name` uses the string variant
and counts whole names such as `Neal Asher`.

To count individual words instead, use `author_name__self`. The `__self`
selector bypasses that default and selects the primary analyzed text:

```http
POST /collections/authors/_search

{
  "query": {
    "all": true
  },
  "limit": 0,
  "get_number": true,
  "ops": {
    "words": {
      "field_facet": {
        "field": "author_name__self",
        "limit": -1
      }
    }
  }
}
```

```json
{
  "found": 6,
  "docs": [],
  "ops": {
    "words": {
      "buckets": [
        {
          "val": "neal",
          "count": 6
        },
        {
          "val": "asher",
          "count": 3
        },
        {
          "val": "stephenson",
          "count": 2
        },
        {
          "val": "shusterman",
          "count": 1
        }
      ]
    }
  }
}
```

An explicit variant selector such as `author_name__s` also bypasses the
default binding. For a text field whose value binding already selects the
primary, the bare field name counts words without needing `__self`.

## Limits

- Facet sorting takes one key, which must be a metric operation of that facet.
  Custom count or bucket-value sort orders are not yet supported; omit `sort`
  for the default count order.
- Integer, date, and text field facets do not accept `ops` or `sort`.
- `selected` is not accepted at the root of a full-form request, under a
  `fusion`, or on a nested facet.
- Range facets with sub-operations are limited to 1,024 buckets; a range facet
  without them may request at most 100,000.
- Facet aggregate state is bounded by the server's
  `search.request-memory-max-bytes` per-request ceiling. The engine charges the
  aggregate state for every simultaneously resident bucket, including metrics
  used to sort candidates.
