# Parent and child search

Relations model products with variants, documents with chunks, and other
one-to-many records inside one collection. `has_child` searches child records
and returns each matching parent once. Named `inner_hits` return the actual
matching children alongside each parent. Children are independently indexed
records; they can live in different segments from their parent.

## Declare a relation

Add `relations` to the collection schema. Each relation has its own field
namespace. The child `id` is implicit and unique within a parent incarnation
and relation. Parent fields retain their ordinary schema names.

```json
{
  "relations": {
    "variants": {
      "fields": {
        "description": {"type": "text", "index": "match"},
        "color": {"type": "string", "column": true},
        "price": {"type": "int", "column": true, "index": "range"},
        "embedding": {"type": "vector", "dims": 4, "metric": "l2", "column": true}
      }
    }
  }
}
```

Fields can use the existing schema types, analyzers, and vector index
configuration. A relation field name cannot be `id`, start with `luxir_`, or
use the engine's reserved underscore namespace. Relations are one level deep.

## Write and replace a family

Send this body to the collection update endpoint, or use the same fields in
the protobuf `UpdateRequest`:

```json
{
  "families": [{
    "parent": {"id": "shirt", "title_s": "Cotton shirt"},
    "children": {"variants": [
      {"id": "red-small", "description": "red cotton", "color": "red", "price": 20, "embedding": [1, 0, 0, 0]},
      {"id": "blue-large", "description": "blue cotton", "color": "blue", "price": 35, "embedding": [0, 1, 0, 0]}
    ]}
  }],
  "commit": {}
}
```

A family replaces the parent and **all** its relations. Omitting a relation
from `children` clears that relation for this parent. Updates containing
`families` or `child_updates` are atomic for the entire request, including
ordinary documents and deletes in that request. Invalid child data rolls
back the parent replacement too. Such requests do not accept `allow_dups`,
`field_map`, or `drop_unmapped`.

A successful unary update response contains `families`, a map from parent IDs to new
opaque incarnation tokens. Keep the token to perform independent child
updates. Search can also retrieve it using `fields: ["id", "_family_"]`.
A normal `docs` overwrite of a parent replaces it without a family and
therefore disconnects its old children. Use family replacement when replacing
an existing family.

## Change individual children

Use the token returned by the family's update:

```json
{
  "child_updates": [{
    "parent_id": "shirt",
    "family": "TOKEN_FROM_UPDATE_RESPONSE",
    "relation": "variants",
    "docs": [{"id": "red-small", "description": "red cotton", "color": "red", "price": 18}],
    "delete_ids": ["blue-large"]
  }],
  "commit": {}
}
```

Each child document is a full replacement of that child. Parents and their
other children are not rewritten. A token from an older incarnation remains
bound to that incarnation: a delayed update using it cannot reattach to a
replacement parent. Such stale writes may be accepted into storage but have
no visible effect. Tokens must originate from an earlier update, and must
identify the supplied parent. This is incarnation binding, not an optimistic
concurrency check against the currently visible parent.

Deleting a parent through `delete_ids` hides its children from joins. Old
incarnations remain physically stored until separately reclaimed; ordinary
segment compaction alone does not garbage-collect these orphan records.

## Match one variant and return the product

```json
{
  "query": {
    "has_child": {
      "relation": "variants",
      "query": {"match": {"field": "description", "val": "cotton"}},
      "filter": [
        {"match": {"field": "color", "val": "red"}},
        {"range": {"field": "price", "lt": 25}}
      ],
      "score_mode": "max",
      "inner_hits": {"name": "matching_variants", "limit": 3, "fields": ["id", "price", "color"]}
    }
  },
  "fields": ["id", "title_s"],
  "get_scores": true
}
```

All predicates inside this `has_child` apply to the **same child**. Two
separate `has_child` clauses can be satisfied by different children of the
same parent. Queries and filters outside the join apply to parent fields.
Regular browsing, counts, and facets operate on parents, not hidden children.

`score_mode` supports `max` (default), `min`, `sum`, `avg`, and `none`.
`none` assigns the constant parent score 1 before an outer boost.
`min_children` defaults to 1; `max_children` defaults to unbounded. Both count
children admitted by the child query, after its filters. Inner-hit limits do
not affect these counts or the parent score. Inner hits are ordered by child
score descending, independently of the parent reducer. For `min`, `sum`, or
`avg`, the first inner hit does not necessarily explain the parent's aggregate
score on its own.

The returned `DocList.matches` array aligns with its parent result rows in
both row and column formats, including streaming batches and offsets. Each
entry maps an inner-hit name to a child `DocList`. That list contains `found`
and row documents with the original child `id`, selected source fields,
`_score_`, and `_relation_`. An absent `fields` list returns the child source.
An absent `inner_hits` disables evidence retrieval; `limit: 0` also disables
it. The default inner-hit limit is 1, and the maximum is 1000. Names must be
unique among matching clauses in the emitted result.

## Vector and hybrid retrieval

Inside `has_child`, kNN defaults to distinct-parent selection. `k` counts
parents, and `unit: "parents"` makes that explicit. `unit: "docs"` opts into
a child candidate window, which can return fewer than `k` distinct parents.
`has_child.filter` restricts child eligibility **before** vector selection;
enclosing parent filters are translated into child eligibility before kNN.

```json
{
  "query": {"has_child": {
    "relation": "variants",
    "query": {"knn": {"field": "embedding", "query": [1, 0, 0, 0], "k": 10, "unit": "parents", "exact": true}},
    "inner_hits": {"name": "nearest_variant"}
  }}
}
```

A grouped vector query admits its best vector child per selected parent.
Its inner hits and child counts describe that admitted window, not every
similar child in the family. Asking for more inner hits does not run another
vector search or fabricate additional witnesses. Approximate execution uses
the existing group-aware vector engine and its underfill warnings.

For hybrid retrieval, put a `has_child` query in each lexical/vector
`fusion.sources` entry and use different inner-hit names. Fusion then ranks
actual parent IDs, even when its sources matched different children.
Only witnesses from contributing source windows are attached. To score text
and vector signals on the same child, combine those clauses inside a single
`has_child` query instead; its filters still apply to individual children.
This combined scoring is limited to the admitted vector candidate window.
Use `unit: "docs"` and an appropriate candidate budget when several children
per parent must compete on their combined score: parent-grouped kNN has
already retained only the vector winner. Neither bounded mode guarantees the
best combined text/vector child outside its candidate window.

## Execution and scope

The reader caches parent-incarnation links and root domains for its immutable
snapshot. Join preparation translates the parent domain, evaluates child
filters and retrieval, reduces scores by parent, and produces ordered parent
scorers. Projection reads stored child source only for emitted witnesses.
Query domains, parent accumulators, and witness lists are charged to the
request memory breaker. The snapshot topology cache is separate and grows
with physical records. These joins deliberately bypass predicate caching until
a key that includes relation identity and domain provenance is implemented.

The current operator is a same-collection parent-returning semijoin. It does
not implement arbitrary cross-collection joins, recursively nested relations,
sibling expansion, child-field sorting of parents, or child-domain facets.
Parent sorting and parent-domain facets remain available. The
[research and implementation proposal](../design/parent-child-search-plan.md)
records those possible extensions and the alternatives considered.
