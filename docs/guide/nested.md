# Nested Documents

A nested field holds a list of owned sub-objects: the variants of a product,
the chunks of a document, the answers under a question. Each element is
written and replaced with its document, and queries can require that several
conditions hold on the same element. Elements are first-class: you can rank
elements (chunks) or documents (products), facet over elements, sort
documents by an element value, and return the elements that matched.

## Declare a nested field

A nested field has `type: "nested"` and a `fields` map of ordinary field
definitions. Child fields take every field type except `id` and `nested`
(one nesting level), with the usual options: analyzers, `multi`, `index`,
vectors and their ANN settings.

```http
PUT /collections/products/_schema

{
  "fields": {
    "title": "text",
    "variants": {"type": "nested", "fields": {
      "color": "string",
      "price": {"type": "int", "index": "range"}
    }},
    "chunks": {"type": "nested", "fields": {
      "text": "text",
      "embedding": {"type": "vector", "dims": 384, "metric": "cosine"}
    }}
  }
}
```

A nested field accepts only `type`, `fields` and `stored`. With `stored`
(the default) the document keeps the submitted list, so retrieval returns it
as written.

## Index documents

Elements are ordinary JSON objects in a list. Array order is preserved.

```json
{"id": "A", "title": "shirt", "variants": [{"color": "red", "price": 10}, {"color": "blue", "price": 12}]}
```

- A missing field or `null` means the document has no list; `[]` is an
  empty list; a `null` element is an element with no values. A single object
  is a list of one element.
- Element fields are validated like document fields. An unknown or malformed
  element field fails the whole document, and none of its elements are
  indexed.
- Overwriting or deleting a document replaces or removes all of its
  elements. Elements have no ids of their own.

## Address element fields

Inside a nested scope, element fields are named `<path>.<field>`, such as
`variants.color`. A short name never falls back between scopes:

- In a document-level query, `variants.color` is an error that points to
  `to_parent`: a predicate on an element field must say which element it is
  about.
- In element scope, `color` alone is an error; write `variants.color`.
- In element scope, `root.<field>` returns the owner's value in a `fields`
  list (for example `root.id`, `root.title`).

## Documents whose elements match: `to_parent`

`to_parent` matches the documents (owners) that have at least one element
matching its query. Every clause inside one `to_parent` applies to the same
element. Separate `to_parent` clauses may be satisfied by different
elements. It is valid wherever a query is valid, including filters and
`prohibited` clauses.

```json
{"query": {"to_parent": {"path": "variants", "query": {"boolean": {"required": [
    {"match": {"variants.color": "red"}},
    {"range": {"field": "variants.price", "lte": 10}}]}}}}}
```

| Field | Meaning |
|---|---|
| `path` | The nested field. |
| `query` | A query over the path's elements, using `<path>.<field>` names. |
| `score` | How element scores combine into the owner's: `max` (default), `min`, `sum`, `avg`, or `none` (0). |
| `min_children`, `max_children` | Bounds on the number of matching elements per owner. |
| `name` | Names the clause, for `row_ops` with `from_match`. |

Element text is scored with statistics of the element row space (document
frequency and average length of element text, not of whole documents).
Phrase and position queries match within one element.

### Vector search over elements

A kNN query inside `to_parent` searches element vectors. By default `k`
counts elements, so several of the nearest elements may belong to one
document. `"unit": "owner"` makes `k` count distinct owners, ranked by their
best element, before any cut:

```json
{"query": {"to_parent": {"path": "chunks", "query": {"knn": {
    "field": "chunks.embedding", "query": [0.1, 0.2], "k": 20, "unit": "owner"}}}},
 "filter": [{"match": {"title": "manual"}}],
 "limit": 20}
```

Document filters apply before candidate selection: they restrict the search
to the elements of eligible documents.

## Operations over elements: `domain.to_children`

An operation whose `domain` has `to_children` runs over the elements of the
documents in its incoming domain. Every operation works there unchanged:
lists, facets, metrics, collapse and diversify. `domain.filter` applies in
element scope. A nested `domain.to_parent: true` projects back to the
distinct owners of the incoming elements.

Top 20 chunks, at most 3 per document, with the document title:

```json
{"ops": {"chunks": {"domain": {"to_children": "chunks"}, "top_docs": {
  "query": {"knn": {"field": "chunks.embedding", "query": [0.1, 0.2], "k": 20}},
  "limit": 20,
  "diversify": {"owner": "root", "max_per_key": 3},
  "fields": ["chunks.text", "root.id", "root.title"]}}}}
```

Colors of the variants of the matching products, under a list:

```json
{"query": {"match": {"title": "shirt"}}, "limit": 10,
 "ops": {"colors": {"domain": {"to_children": "variants"},
                    "field_facet": {"field": "variants.color"}}}}
```

`"owner": "root"` in `diversify` or `collapse` keys elements by their
document with no key field. Scope changes are supported at the request root
and under `top_docs`; `domain.query` does not combine with them, and fusion
does not change scope.

## Sort documents by element values

From a document's side, an element field in an expression is the array of
that document's element values, so the array reducers apply:

```json
{"sort": [{"expr": "min(variants.price)"}]}
```

`min`, `max`, `avg`, `sum` and `count` work over numeric element columns.
Documents without elements sort as missing (last). A bare `variants.price`
needs a reducer.

## Return elements

`fields` on a document list can return:

- `variants`: the stored list as submitted;
- `variants.color`: the list of that element field's values, one per
  element (`null` where an element lacks it).

### Per-row operations: `row_ops`

`row_ops` runs a small `top_docs` over each returned row's own elements and
returns it as the row's value under the operation's name.

- `domain.to_children: "<path>"` selects all of the row's elements.
- `domain.from_match: "<name>"` selects the elements a named `to_parent`
  clause of the list's query matched, scored by that clause.

A row operation takes `query`, `filter`, `sort`, `limit`, `offset`,
`fields` and `diversify`. A query in a row operation is a new computation
over the row's elements.

```json
{"query": {"to_parent": {"path": "chunks", "name": "hits", "query": {"match": {"chunks.text": "battery"}}}},
 "limit": 10, "fields": ["id", "title"],
 "row_ops": {"matching_chunks": {"domain": {"from_match": "hits"},
                                 "top_docs": {"limit": 3, "fields": ["chunks.text"]}}}}
```

`from_match` does not replay a clause that contains kNN: its admitted
window is global, and a per-row replay would be a different search. Use
`domain.to_children` with an explicit `knn` query instead.

## Limits

- One nesting level.
- Elements are rewritten with their document. Partial updates of element
  attributes are planned separately.
- Filters inside element-scope operations are not cached across requests
  yet (`to_parent` membership is cached like any other owner filter).
- Element vector fields are searched exactly (no ANN overlay yet).
- `from_match` replays non-kNN clauses only.
