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
