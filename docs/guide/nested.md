# Nested documents

A document can contain lists of structured children: an order with line items,
a product with variants, an article with chunks. Flattening such a list loses
which values belonged together. Given

```json
{"id":"a","items":[{"color":"red","size":"S"},{"color":"blue","size":"L"}]}
{"id":"b","items":[{"color":"red","size":"L"}]}
```

a flattened index sees `color: [red, blue]` and `size: [S, L]` on document a,
so "a red item in size L" matches it even though no such item exists.

A **nested path** keeps each child as its own row. A query can then ask for
one child that satisfies several conditions, score a document by its best
child, facet over the children of the matching documents, and search text or
vectors that belong to a child. Children can have children, up to 32 levels.

Root documents are not affected by any of this: a collection without nested
paths, and a request that names none, work exactly as before.

## Declaring nested paths

A nested path is a field of type `nested`. Its fields are declared by flat
qualified names; a dot separates path components:

```json
{"fields":{"items":"nested","items.color":"string","items.size":"string",
           "items.parts":"nested","items.parts.material":"string"}}
```

- Each intermediate component must itself be a nested path.
- Suffix templates work under a path. In the default schema `_nested` declares
  a path, so `lines_nested.color_s` needs no explicit declaration.
- A nested path is structural: it has no value settings, variants, index,
  column or storage of its own.
- Each component starts with an ASCII letter and contains letters, digits or
  underscores. A physical name, including a generated `__variant` suffix, must
  fit in 127 bytes.

A nested path stores a reverse map from each child row to its owner, about
one byte per child row. It speeds up joins led by a selective child query,
whose matching rows are scattered across owners. `"items": {"type": "nested",
"reverse_map": false}` leaves it out; each nested path has its own setting and
a template can carry it. A change applies to segments written afterwards,
merges included, and never changes results.

See [Schema and fields](schema.md#nested-paths-and-qualified-field-names) for
where these declarations sit in a schema.

## Indexing

Send nested values as ordinary JSON, an object or an array of objects:

```json
{"id":"a","items":[{"color":"red","size":"S","parts":[{"material":"steel"}]},
                   {"color":"blue","size":"L"}]}
```

- Each object creates one child row, including an empty object `{}`. A single
  object counts as one row.
- Null, an empty array and an absent field create no rows.
- A `null` element of an array is an empty child row, like `{}`, at every
  depth. It is returned as `{}`.
- A scalar, or an array element that is neither an object nor `null`, is a
  document error. Any error inside a child rejects its whole root document.
- Supply child keys inside their objects. A dotted key at the root
  (`"items.color": "red"`) cannot create a child row. `field_map` and
  `drop_unmapped` apply to root keys only.
- Children have no identity of their own. Delete and overwrite are by root
  `id`, and they hide the old child rows with the root.

## Querying

### Any child, or the same child

A query may name a child field directly. Each such clause is satisfied by any
child of the document, independently of the other clauses:

```
items.color:red AND items.size:L
```

matches both documents above: some item is red, and some item is L.

A group on a nested path is evaluated against one child at a time:

```
items:(color:red AND size:L)
```

matches only b, because one item must satisfy both clauses. Inside the group,
names are relative to the path. Unlike a group on a scalar field such as
`title_t:(red OR blue)`, a nested-path group does not distribute over its
clauses. With one clause the two forms are the same query:
`items:(color:red)` is `items.color:red`.

### Has children, and negation

`items:*`, or `{"exists":{"field":"items"}}`, means "has at least one item".
An empty object is a row and matches; an absent, null or empty-array path does
not.

Negation depends on where it sits:

| Query | Meaning |
|---|---|
| `NOT items.color:red` | no item is red (also true with no items) |
| `items:(NOT color:red)` | some item is not red, even if another item is |

### Deeper levels

A dot always descends, and groups nest:

```
items.parts.material:steel
items:(parts:(material:steel) AND color:red)
```

The first matches documents where some part of some item is steel. The second
requires a red item that itself has a steel part.

### Structured form

The structured equivalent of a group is `to_parent`. Its `path` and the field
names inside it are relative to the view where it appears:

```json
{"to_parent":{"path":"items","score":"max","query":{
  "boolean":{"required":[{"match":{"field":"color","val":"red"}},
                         {"match":{"field":"size","val":"L"}}]}
}}}
```

A structured leaf that names a child field directly (match, any_of, phrase,
range, prefix, wildcard, regex, fuzzy, exists, geo, kNN) joins implicitly, the
same way an expression does. Use combined `multi_match` within one view only;
`per_field` mode can combine individually joined fields from different views.
Wildcards can select leaf fields within a literal path (`items.te*`); wildcard
path components are not supported.

### Scoring

A document's score from a nested clause is the score of its best matching
child. That is `score: "max"`, the default, and what implicit references and
nested-path groups always use. The structured `to_parent` form takes the
other reductions:

| `score` | The document's score from the clause |
| --- | --- |
| `max` (default) | highest score among its matching children |
| `min` | lowest score among its matching children |
| `sum` | sum of its matching children's scores |
| `avg` | average of its matching children's scores |
| `none` | zero: the clause selects documents without ranking them |

A boost on the join multiplies the reduced score. Every mode matches the same
documents; a mode other than `max` needs the score of every matching child,
so `max` is the cheapest ranked mode.

`min_children` and `max_children` bound how many children must match:

```json
{"to_parent":{"path":"items","score":"avg","min_children":2,"max_children":5,
              "query":{"match":{"field":"color","val":"red"}}}}
```

matches documents with two to five red items and scores each by the average
over all of its red items. The defaults are 1 and unbounded. `min_children`
must be at least 1; for documents with no matching child, negate the clause.

A dotted path (`"path":"items.parts"`) reduces over all of the document's
matching rows at that path, and its bounds count those rows. A join written
inside a join reduces and counts level by level instead, which can give a
different average or a different count.

### As a filter

A nested clause works anywhere a query does: main query, filter, prohibited
clause, or a domain filter. As a filter it restricts documents without
changing their relevance:

```json
{"query":"body_w:camera", "filter":["items:(color:red AND size:L)"],
 "fields":["id"], "count_limit":-1}
```

Filter results of nested clauses are cached like other filters.

## Operations on children: `domain.to`

Queries can reach into children implicitly, and returned fields can reach
children and owners (see [Returning nested values](#returning-nested-values)).
Facets, collapse keys, and direct field sorts read the fields of their own
view; collapse and diversify can also
[key rows by their document](#keying-child-rows-by-their-document). Value expressions can reduce descendant values (see below). To run an
operation over child rows themselves, move its domain:

```json
{"query":"body_w:camera", "filter":["items:(color:red AND size:L)"],
 "ops":{"colors":{"domain":{"to":"items"},
                  "field_facet":{"field":"color"}}}}
```

This facet counts the item rows of the matching documents, including their
other items. To restrict the rows themselves, add a `domain.filter`, which is
resolved in the destination view: `"filter":["color:red AND size:L"]`.

`to` is the one navigation key:

| `to` | Destination |
|---|---|
| `items`, `items.parts` | a descendant of the current view |
| `..` | the immediate owner |
| `../..` | two levels up |
| `/` | root documents, including when already there |

The slash separates `..` steps only. `../reviews` (up, then down another path)
is reserved and rejected; use separate operations for different child paths.
`..` above the root and an empty `to` are errors. A valid path with no indexed
rows is an empty view.

Operations can move more than once. Below an operation over parts, these
sub-operations count the items and the documents that the matching parts
belong to:

```json
{"domain":{"to":"items.parts"},"top_docs":{"limit":0,"ops":{
  "items":{"domain":{"to":".."},"top_docs":{"count_limit":-1,"limit":0}},
  "documents":{"domain":{"to":"/"},"top_docs":{"count_limit":-1,"limit":0}}
}}}
```

## Keying child rows by their document

In a child view, `"owner": "root"` in `collapse` or `diversify` keys each row
by the root document that owns it. No field is named, so the children need no
copy of the document's id. The top 20 chunks, at most 3 per document:

```json
{"ops":{"chunks":{"domain":{"to":"chunks"},"top_docs":{
  "query":{"knn":{"field":"embedding","query":[0.1,0.2],"k":100}},
  "limit":20,"fields":["text","/id","/title"],
  "diversify":{"owner":"root","max_per_key":3}}}}}
```

With `collapse`, the same key returns one row per document, its best child,
and `get_count` reports how many of its children matched:

```json
{"ops":{"best":{"domain":{"to":"items"},"top_docs":{
  "filter":["color:red"],"sort":[{"field":"price"}],"fields":["price","/id"],
  "collapse":{"owner":"root","get_count":true}}}}}
```

- `root` is the only owner. At a deeper path such as `items.parts` it is still
  the root document, not the item.
- `field` may be omitted. When given, the owner is one more component of the
  key tuple: `{"owner":"root","field":"color"}` keeps one row per document
  and color.
- A row always has an owner, so `missing` only concerns the named fields.
- Counts, paging, `head_sort`, `ops` and `sort` behave as with a field key.
  `found` counts documents under `collapse`; `found_docs` counts child rows.
- The returned group value is the owner's row number in the current index
  snapshot. It changes when segments merge; use a returned `/id` to identify
  the document.
- A facet's `count_distinct` accepts the same key, counting documents instead
  of child rows per bucket.
- Outside a child view the option is an error. An owner's field is still not
  a key: `"field":["/id"]` is rejected.

## Reducers over child values

A descendant field in a value expression is an array of its values from all
children owned by the current row. For example, sort questions by their newest
answer:

```json
{"limit":10,"fields":["id"],"count_limit":10000,
 "query":{"match":{"field":"tag","val":"java"}},
 "sort":[{"expr":"max(answers.date)","dir":"desc"}]}
```

Names are relative to the current view: `parts.price` in the items view, or
`items.parts.price` at the root. Values follow descendant row order, and a
multi-valued child field contributes all its values. The array includes all
owned descendants, regardless of which children matched a query.

Use the usual `min`, `max`, `avg`, `sum`, and `count` reducers. A scalar
position requires a reducer; a bare descendant field cannot be a sort key.
No children, or no values among them, means missing, except `count`, which
returns zero. Empty values sort last in either direction. DATE and DECIMAL
values retain their usual expression semantics.

The same expressions work in rescoring and metrics. For example,
`sum(max(items.price))` sums each root's maximum item price over the operation's
domain. Upward names in value expressions are not supported. Facet and
collapse field keys still require `domain.to` to operate in the child's view.

## Names are relative to the view

Every name in a request is resolved in the view where it appears. In the
`items` view, `color` is `items.color` and `parts.material:steel` joins
matching parts back to items. A dot always descends. Only returned-field
selectors can name an owner's field; no name reaches sideways.

Returned keys are relative too. A child view returns its own values under
local names, and `fields: ["*"]` selects only the fields of that view. The
virtual `_version_` is available in child views and reports the owning root's
version.

## Returning nested values

`fields` can return related rows without changing the query or the view of
the result.

| Selector | Returned value |
|---|---|
| `*` | the view's own fields; never descends into a nested path |
| `items` | an array with one object per item, in row order, each with its whole subtree |
| `items.color` | an array with one object per item holding its `color`; an empty object where it is missing |
| `items.color`, `items.size` | one array, with both selections in each object |
| `items.parts.material` | item objects holding part arrays that select `material` |
| `items.*` | each item's own fields, without the nested paths below it |
| `../name` | the immediate owner's field, under the key `../name` |
| `../../name` | a field two levels up, under that key |
| `/name` | the root document's field, under the key `/name` |
| `../*`, `/*` | the owner's or root's own fields, each key prefixed as written |

Return item data beside each root hit:

```json
{"ops":{"q":{"top_docs":{"query":"title_t:shirt",
  "fields":["id","items.color","items.size","items.parts.material"]}}}}
```

Return the item color and the document id with each matching part:

```json
{"ops":{"q":{"domain":{"to":"items.parts"},"top_docs":{
  "query":"material:steel","fields":["material","../color","/id"]}}}}
```

- A nested path is returned only when named. Its array holds all children of
  the hit, including those that did not match the query.
- The array is rebuilt from the indexed child rows with the same retrieval
  rules as a query in that view. It is not a copy of the submitted value:
  field order, unretrievable values and the difference between an absent,
  null and empty list are not restored. An empty object is a row and comes
  back as an empty object.
- A document with no rows on the path has no such key in row output; column
  output shows a null cell.
- An upward selector names a field or a pattern. `..` above the root, a bare
  `..` or `/`, and up then down (`../reviews.rating`) are rejected.
- There is no cap on the number of children returned per hit. Large families
  make large responses, and their size counts against the request memory
  limit.
- `_version_` on any row is its owning root's version. Scores of owners and
  children are not returned.

## Matched children per hit

Name the descendant clause and reference it from a row operation. This
returns only the matching answers for each returned root, rather than every
answer as a returned-field selector would.

```json
{
  "limit": 10, "fields": ["id"], "count_limit": 10000,
  "query": {"boolean": {"required": [
    {"match": {"field": "tag", "val": "java"}},
    {"name": "recent", "range": {
      "field": "answers.date", "lte": "2012-04-08T21:15:33.873Z"}}
  ]}},
  "row_ops": {"inner": {
    "domain": {"from_match": "recent"},
    "top_docs": {"limit": 3, "fields": ["date", "user"]}
  }}
}
```

A name may mark an explicit `to_parent`, a nested-path expression group, or
an implicit descendant leaf. Each row operation sees that root's own family.
Without a query of its own, the operation uses the named clause's child-side
query for matching and scoring. An explicit query instead scores within the
named clause's matching rows. A `domain.filter` narrows either form further.
To retrieve children independently of a matching clause, use `domain.to`;
with no query those children are unscored and in row order.

When the named clause is a join with `min_children` or `max_children`, the
bounds decide which documents are hits and `from_match` still selects every
matching child of each hit.

A named kNN clause replays as an exact ranking of each hit's children by the
clause's query vector; see [Matched chunks](#matched-chunks).

See [Per-row operations](searching.md#per-row-operations) for controls,
restrictions, formats and result-name rules.

## Chunks and vectors

Text chunks are a natural use: `chunks_nested:(text_wl:"quick fox")` requires
the phrase within one chunk, and words split across sibling chunks do not form
a phrase.

A kNN clause's `k` counts rows of the view where the clause is written. At
root, a child vector field selects distinct documents by their best child:

```json
{"query":{"knn":{"field":"chunks_nested.embedding_v","query":[1,0],"k":10}}}
```

Inside an explicit join, the same search selects child rows before the join
reduces them, so it may match fewer than ten documents:

```json
{"query":{"to_parent":{"path":"chunks_nested","query":{
  "knn":{"field":"embedding_v","query":[1,0],"k":10}}}}}
```

A list with `domain.to: chunks_nested` also counts child rows. At deeper paths,
`k` counts roots, items or parts according to the view where the clause is
written. Filters and deletes decide which rows of that view are eligible
before the search, and an owner with no vectors is never a neighbor.

An owner is scored like a document of a
[multi-valued vector field](vector-search.md#define-a-vector-field): by its nearest
child with `exact: true`, and by the nearest of its examined children in an
approximate search. [Candidate budgets](vector-search.md#query-knobs) count
owners where they count documents; with PQ or an explicit `refine_candidates`,
one owner with thousands of near vectors can crowd the budget and leave fewer
than `k` owners.

Vector ANN indexes can be built on child fields using their qualified names,
for example `vec.lines_nested.embedding_v`.

### Matched chunks

Name the kNN clause and replay it per hit to return each document's chunks
ranked by the same similarity. The top ten documents by their best chunk,
each with its three nearest chunks:

```json
{"limit":10,"fields":["id","title_s"],
 "query":{"name":"near","knn":{"field":"chunks_nested.embedding_v",
                               "query":[1,0],"k":10}},
 "row_ops":{"passages":{"domain":{"from_match":"near"},
   "top_docs":{"limit":3,"fields":["text_wl"],"get_scores":true}}}}
```

The replay scores every chunk of the document exactly, so the first passage
is the chunk that ranked the document, and its score is the document's score.
It keeps the clause's `similarity` threshold and boost. A `domain.filter`
restricts the passages (for example to one language), and an explicit
`top_docs.query` ranks them by text instead while the kNN still decides which
chunks qualify. See [Per-row operations](searching.md#per-row-operations).

The same works beside a collapsed, diversified or fused list, and
`fusion.row_ops` can replay a clause of any source: a hybrid search can fuse
a text query and a kNN over chunks and return the matching chunks of each
fused document from either side.

### Chunks capped per document

To rank chunks themselves while limiting each document's share, search the
chunk view and key it by the [owning document](#keying-child-rows-by-their-document):

```json
{"ops":{"chunks":{"domain":{"to":"chunks_nested"},"top_docs":{
  "query":{"knn":{"field":"embedding_v","query":[1,0],"k":100}},
  "limit":20,"fields":["text_wl","/id","/title_s"],
  "diversify":{"owner":"root","max_per_key":3}}}}}
```

The cap applies after the kNN cut, so pick `k` large enough to fill the page.
To have `k` count documents in the chunk view instead, collapse on the owner
and give the kNN `"unit":"groups"`: each document then contributes its best
chunk.

Ordinary multi-valued vector fields already cover plain chunking, where a
chunk is only a vector. Use nested rows when the text, vectors or other fields
of each child matter together.

## Deletes, merges and limits

- A child is live exactly when its root is. Deleting or replacing a root hides
  its children at once.
- Segments with nested rows merge like any others, and the child rows of
  deleted roots are dropped with them. A merge level counts a segment's
  largest row space.
- A segment is left out of a merge that would take one row space past the
  per-segment row limit. That check counts physical rows, including those of
  deleted roots.
- Followers replicate nested rows with their segments.

## Not supported yet

- Storing the submitted nested value verbatim.
- Owner fields in sorts, facets, collapse keys and value expressions. Collapse,
  diversify and `count_distinct` can key child rows by their root document
  with `owner`.
- Updating or addressing a single child.
- `domain.to` paths that go up and then down.
