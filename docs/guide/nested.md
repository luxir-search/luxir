# Nested objects

A `nested` field stores an array of objects so that each object can be matched
as a unit: a query on two child fields matches only when one object satisfies
both. Each object is indexed as a hidden child document, written next to its
root document in one contiguous block.

Use `nested` when objects are always replaced together with their parent, as
with a question and its answers. Use [relations](relations.md) when children
change independently of the parent, or when a parent has many thousands of them.

## Declare a nested field

```json
{
  "fields": {
    "answers": {
      "type": "nested",
      "fields": {
        "user": {"type": "string"},
        "score": {"type": "int", "column": true, "index": "range"},
        "body": {"type": "text", "index": "match"}
      }
    }
  }
}
```

Child fields use the ordinary field types and are addressed by path, for
example `answers.user`. Paths are one level deep: a child field cannot itself
be `nested`.

## Index documents

A nested field takes one object or an array of objects:

```json
{"docs": [{"id": "q1", "answers": [
  {"user": "alice", "score": 5, "body": "use a merge"},
  {"user": "bob", "score": 1, "body": "try again"}
]}]}
```

Objects are checked against the declared fields. An unknown child field, or a
value that is not an object, rejects that document as a whole, and it is
reported at its own index in `errors` without affecting other documents in the
request. A failure partway through a block removes every record of the block.

## Blocks are the unit of change

Deletes and updates address the root document, and the whole block goes with
it: deleting `q1` removes its answers, and writing `q1` again replaces them.
Children cannot be updated independently. Merges keep each block contiguous.

Counts, match-all, and facets over the root documents see only roots. The
`stats` document counts report physical records, children included.

## Returned fields

Returned documents keep the original structure: `answers` is returned as the
array of objects that was written, including fields stored only for retrieval.

## Query nested objects

The `nested` query matches within one path and returns root documents. Clauses
inside it apply to a single child, so a query on two child fields cannot match
across different children:

```json
{"query": {"nested": {
  "path": "answers",
  "query": {"boolean": {"required": [
    {"match": {"field": "answers.user", "val": "alice"}},
    {"range": {"field": "answers.score", "gte": 5}}
  ]}},
  "score_mode": "avg",
  "inner_hits": {"name": "matching", "limit": 3, "fields": ["user"]}
}}}
```

`score_mode` is `avg` (default), `max`, `min`, `sum`, or `none`. `filter` adds
non-scoring child clauses. `inner_hits` returns the matching children's original
objects in `DocList.matches`, as with relations. A `nested` query cannot contain
<<<<<<< HEAD
another `nested` or `has_child`, or `knn`.

## Aggregate over nested objects

The `nested` operation re-points the current documents at a path's child
documents and runs its sub-operations there; `reverse_nested` points them back
at the root documents. Each returns a `DocList` with `found` and `ops`.

```json
{"ops": {"q": {"top_docs": {"query": {"all": true}, "ops": {
  "n": {"nested": {"path": "answers", "ops": {
    "by_user": {"field_facet": {"field": "answers.user"}},
    "questions": {"reverse_nested": {"ops": {
      "by_title": {"field_facet": {"field": "title_s"}}
    }}}
  }}}
}}}}}
```

## Sort by a nested field

Add `nested` to a sort clause to order root documents by a child field:

```json
{"sort": [{"expr": "answers.score", "dir": "asc",
           "nested": {"path": "answers", "mode": "min"}}]}
```

The field must be single-valued numeric or date. `mode` is `min` (default for
ascending), `max` (default for descending), or `sum`/`avg` on integer fields.
Documents without a value in the path sort last. Sorting by a string field and
filtering the children that participate in the sort are not yet supported.
=======
another `nested` or `has_child`, or `knn`. Clauses inside one match child
documents only; a clause on a root field matches nothing.
>>>>>>> nested/query
