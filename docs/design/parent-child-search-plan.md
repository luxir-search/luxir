# Parent and child search implementation proposal

Status: research proposal, with the core same-collection relation path now
implemented on `feature/parent-child-search`. Research and source review dated
2026-10-27; Luxir baseline `609367c5`. The [implemented API and its current
limits](../guide/relations.md) are authoritative. The examples and milestones
below preserve the broader proposal, including extensions not implemented by
the initial relation operator.

The implementation uses ordinary independently keyed physical records, a
snapshot-local incarnation map, whole-reader join preparation, grouped kNN,
and prepared-result evidence. It supports atomic family replacement,
incarnation-bound child mutations, parent scope isolation, exact score/count
reducers, and lexical/vector parent fusion. It does not yet implement child
sort/facet operators, sibling expansion, recursive joins, orphan reclamation,
or a packed-block storage backend. Tokens are explicit on child mutations;
stale writes remain invisible rather than receiving an optimistic conflict.

Implement native, typed, same-collection parent/child relations. Search child
records, rank and return actual parent records, and retain the children that
explain each match. Use independently stored, keyed children first. Keep
physical block storage as a later optimization for families normally replaced
together, rather than making adjacency a requirement of the public model.

This covers products with variants, documents with passages, organizations
with locations, and similar one-to-many relationships. The query operation is
a parent-returning semijoin: each qualifying parent appears once, with a
bounded set of matching children. A SQL inner join would instead produce one
row per qualifying pair. General cross-collection joins and arbitrary graph
traversal are separate features.

The key changes are in identity, query scope, candidate selection, and evidence
retrieval. Adding a nested JSON decoder or fetching a parent after ordinary
child top-k would not provide these semantics.

## Required behavior

| Use case | Match and rank | Return |
| --- | --- | --- |
| Product variants | One variant satisfies description, price, color, size, and availability constraints together; rank products by their best qualifying variant or a specified matching-variant metric. | Actual product, selected SKU, and optionally other qualifying SKUs. |
| Document chunks | Search chunk text or embeddings; select distinct eligible documents during retrieval. | Actual document, chunk ID, text, offsets, and score. |
| Document hybrid search | Combine lexical and vector evidence at the document level, even when different chunks provide the evidence. | Document plus witnesses identified by retrieval source. |
| Same-child hybrid search | Compute the combined score for each eligible child before selecting the parent's best child. | Parent and the child attaining that combined score. |
| Multiple child conditions | Explicitly distinguish one child satisfying both conditions from two possibly different children satisfying them. | Parent and witnesses for each named condition. |
| Related-record display | Fetch additional siblings without claiming they caused the match. | Separately named related records. |

Define these invariants before implementing optimizations:

1. A parent must exist and be live in the same pinned snapshot as its children.
2. A child belongs to exactly one parent and one declared relation in v1.
3. A child predicate evaluates all its fields on that child. Parent predicates
   evaluate parent fields. No implicit mixing of their document-ID spaces.
4. Parent eligibility and required child filters apply before bounded vector
   selection. Late parent filtering cannot silently consume the requested k.
5. The returned parent is the stored parent record, not a selected child with
   its ID relabeled.
6. The ranking witness must survive collection and fusion. It must satisfy the
   predicates that admitted it, including any vector candidate-window rules.
7. Counts, facets, sorting, cache keys, and result completeness identify their
   scope explicitly.

## What Luxir already supplies

The current engine has much of the retrieval machinery:

| Existing component | Reuse and required change |
| --- | --- |
| [KnnQuery](../../src/luxir/query/KnnQuery.h), [VectorEngine](../../src/luxir/query/VectorEngine.h) | Global preparation, flat/IVF execution, filtering, candidate widening, full-precision refinement, and group-aware admission already exist. Generalize the grouping identity to an actual parent relation. |
| [GroupKnnTest](../../test/basic_tests/GroupKnnTest.cpp) | Existing oracles cover distinct groups, skew, multiple segments, deletes, ANN admission, and witness-only domains. Extend rather than replace these contracts. |
| [Collapse](../../src/luxir/search/Collapse.h), [CollapseMembers](../../src/luxir/search/CollapseMembers.h) | Key decoding, page-member lookup, and bounded enrichment are useful primitives. Collapse currently groups peers; it does not resolve a unique parent record. |
| [FusionOp](../../src/luxir/search/ops/FusionOp.h) | Fusion accumulates by `segdoc`. If each source first returns actual parent `segdoc`s, existing fusion can combine their scores correctly. Preserve source-specific child evidence beside the scores. |
| [Query and Weight](../../src/luxir/query/Query.h) | `Weight::prepare` supports whole-reader work followed by per-segment replay. This is the correct initial seam for a join whose children and parents can be in different segments. |
| [IndexReader](../../src/luxir/search/IndexReader.h), [OrdMap](../../src/luxir/search/OrdMap.h) | Snapshot ownership, segment reuse, dictionaries, and ordinal translation are useful foundations. A relation also needs a unique live-parent locator and relation-aware liveness. |
| [ProtoUpdateMessage](../../src/luxir/server/ProtoUpdateMessage.cpp), [IdHandler](../../src/luxir/index/handler/IdHandler.h) | Per-document rollback and versioned ID deletion exist. Family identity, child mutation ordering, and family-level atomicity do not. |
| [SegmentMerger](../../src/luxir/index/SegmentMerger.h) | Existing column/postings merge and document remapping can preserve keyed relation metadata. Block layout would require additional invariants. |

Two current behaviors deserve special attention. `knn.unit: groups` produces
one scored witness per selected group, not the group's complete children.
Also, `FusionOp::doFusionImpl` combines child `segdoc`s before optional collapse.
Its own comment identifies the case where text and vector matches land on
different variants. Parent projection must occur before parent-level fusion.

Multi-valued vectors already score a document by its best vector. They do not
provide general correlated chunk text, attributes, IDs, and mutation semantics.
Keep that representation useful for vector-only data; do not make parallel
arrays the primary child-document model.

## Findings from existing systems

### Lucene and Elasticsearch

Lucene's block join stores children before their parent in one atomic block.
A parent bitset maps child positions to parents, and the resulting query can
compose with parent-space clauses. The implementation preserves two-phase
verification; non-max scoring modes force complete child scoring, while max
can retain competitive-scoring opportunities. These are useful execution
patterns, but depend on physical adjacency.
[Lucene 10.3.1 implementation](https://github.com/apache/lucene/blob/releases/lucene/10.3.1/lucene/join/src/java/org/apache/lucene/search/join/ToParentBlockJoinQuery.java).

Lucene's diversifying vector collector maintains a heap indexed by parent,
replacing a parent's child when a better child arrives. Its public vector
query returns winning child IDs for subsequent parent projection. This agrees
with Luxir's existing group-aware direction: diversity belongs in candidate
admission, not only in final presentation.
[Collector implementation](https://github.com/apache/lucene/blob/releases/lucene/10.3.1/lucene/join/src/java/org/apache/lucene/search/join/DiversifyingNearestChildrenKnnCollector.java),
[vector query contract](https://lucene.apache.org/core/10_2_0/join/org/apache/lucene/search/join/DiversifyingChildrenFloatKnnVectorQuery.html).

Lucene also supplies a second query that restricts child matching to a selected
parent's block. This supports separating initial parent selection from fetching
page evidence.
[ParentChildrenBlockJoinQuery](https://github.com/apache/lucene/blob/releases/lucene/10.3.1/lucene/join/src/java/org/apache/lucene/search/join/ParentChildrenBlockJoinQuery.java).

Elasticsearch exposes parent-diversified nested kNN, parent and child filters,
max child scoring, and inner hits for the winning passage. Its documented
restrictions around some sibling-nested filters show why scope and preparation
must be designed explicitly rather than inferred from a JSON path.
[Nested kNN documentation](https://www.elastic.co/docs/solutions/search/vector/knn/nested-knn-search).

For non-block joins, Lucene gathers matches by global ordinal and optionally
maintains scores and occurrence counts. Elasticsearch warns that rebuilding
global ordinals can increase refresh time or create first-query latency spikes.
Luxir should therefore expose relation-build cost, use sparse query state, and
share immutable segment metadata rather than rebuilding a dense child-to-parent
table on every refresh.
[Ordinal collector implementation](https://github.com/apache/lucene/blob/releases/lucene/10.3.1/lucene/join/src/java/org/apache/lucene/search/join/GlobalOrdinalsWithScoreCollector.java),
[JoinUtil contract](https://lucene.apache.org/core/10_3_1/join/org/apache/lucene/search/join/JoinUtil.html),
[Elasticsearch join documentation](https://www.elastic.co/docs/reference/elasticsearch/mapping-reference/parent-join).

### Solr

Solr documents the strength and cost of indexed families directly: nested
relationships accelerate queries, but updates generally require reindexing the
whole tree, apart from its supported in-place updates. This is attractive for
document ingestion and less attractive for frequent SKU changes. Luxir should
not impose that update amplification on both workloads.
[Indexing nested documents](https://solr.apache.org/__root/docs.solr.apache.org/guide/solr/9_7/indexing-guide/indexing-nested-documents.html).

### Qdrant and Vespa

Qdrant groups chunks by a document key, returns several hits per group, and can
look up shared parent payload in another collection. That is a useful result
shape, but a payload lookup alone does not establish parent-side eligibility
before retrieval. Its group implementation iterates a budgeted group driver
and hydrates result payload afterward, reinforcing bounded selection followed
by projection.
[Grouping and lookup](https://qdrant.tech/documentation/search/search/),
[group execution source](https://github.com/qdrant/qdrant/blob/master/lib/collection/src/grouping/group_by.rs)
(moving branch inspected on the research date).

Vespa supports multiple vectors per document and exposes the closest vector's
label through `closest`. This is a useful precedent for preserving the precise
winning vector identity, even when several vectors belong to one child.
Its structured-field facilities also distinguish matching within an array
element from matching unrelated values.
[Multi-vector indexing](https://blog.vespa.ai/semantic-search-with-multi-vector-indexing/),
[closest feature](https://docs.vespa.ai/en/reference/ranking/rank-features.html),
[structured-field schema reference](https://docs.vespa.ai/en/reference/schemas/schemas.html).

## Research implications

The following are design deductions from the papers, not benchmark claims
about Luxir.

| Research | Relevant result | Consequence for this implementation |
| --- | --- | --- |
| [Fagin, Lotem, Naor, Optimal Aggregation Algorithms for Middleware](https://www.wisdom.weizmann.ac.il/~naor/PAPERS/middle_agg.pdf), PODS 2001 / JCSS 2003 | Threshold algorithms establish top-k under monotone aggregation and specific sorted/random-access assumptions. | Exact early stopping needs an upper bound on every unseen contribution. An arbitrary ANN stream or fixed oversampling factor does not supply that guarantee. |
| [Ding and Suel, Faster Top-k Document Retrieval Using Block-Max Indexes](https://research.engineering.nyu.edu/~suel/papers/bmw.pdf), SIGIR 2011 | Block score bounds allow safe pruning while preserving exhaustive results. | Child score bounds can help a max-child parent score, but arbitrary joins do not inherit child document-order bounds. Prove each parent-space bound or disable that optimization. |
| [Cormack, Clarke, Buttcher, Reciprocal Rank Fusion](https://cormack.uwaterloo.ca/cormacksigir09-rrf.pdf), SIGIR 2009 | RRF combines rankings of the same objects without requiring raw-score calibration. | For parent retrieval the objects must be parents. Deduplicate and rank parents per source before fusion; retain source witnesses separately. |
| [Li et al., PARADE](https://arxiv.org/html/2008.09093v2), 2020/2021 | Aggregating passage representations can outperform maximum-passage scoring for broad information needs; localized needs can favor simpler aggregation. | Default to max for retrieval, but keep several evidence passages and a parent-reranking seam. Do not present max as universally optimal relevance. |
| [Khattab and Zaharia, ColBERT](https://arxiv.org/html/2004.12832v2), SIGIR 2020 | Late interaction sums, over query token vectors, their maximum similarities to document token vectors. | Token-level multi-vector retrieval is different from one query embedding selecting the best chunk. Keep it a separate scoring contract. |
| [Dhulipala et al., MUVERA](https://arxiv.org/html/2405.19504v2), 2024, revised 2026 | Fixed-dimensional encodings approximate multi-vector Chamfer similarity, followed by candidate reranking. | A possible future candidate generator for late interaction, not a replacement for relation identity, correlated predicates, or match evidence. Use the revised paper's corrected theoretical bounds if pursuing it. |

Max child score avoids directly summing rewards for having many variants or
chunks. It does not eliminate statistical length bias: more chunks still give
a document more chances to obtain a high score. Measure quality by family size
and retain the ability to rerank with richer evidence.

## Storage decision

| Approach | Benefit | Main cost | Decision |
| --- | --- | --- | --- |
| Flatten fields into arrays | Minimal new machinery | Loses child correlation and identity | Reject for these use cases. |
| Duplicate parent fields into every child and collapse | Uses current engine | Parent updates fan out; no native parent existence or evidence contract | Keep as a migration baseline only. |
| Store only several vectors on the parent | Compact vector-only representation | Does not solve correlated text/attributes or independent child updates | Retain for existing vector-only workloads. |
| Contiguous children and parent blocks | Cheap navigation and good locality | Family rewrites, block-safe flushing/merging, giant-family handling | Evaluate later for replacement-heavy ingestion. |
| Keyed children in the same collection | Independent updates, one snapshot, reuse of columns/postings | Parent resolution, cross-segment preparation, metadata memory | Implement first. |
| Children in a separate collection | Independent retention and scaling | Multiple snapshots, routing, authorization and distributed planning | Defer; not needed for the requested first release. |

### Logical model and identity

Add declared relations to collection schema, each with its own child field
namespace. Start with root-to-child relations; reject cycles and deeper paths
until explicitly implemented. Several relations, such as `variants` and
`reviews`, may share one root type.

Store parent and child records in the same collection's immutable segments.
They may be in different segments. Every record has a scope: root or a declared
relation. Root `all`, ordinary searches, retrieval wildcards, and logical
document counts exclude child records by default. Administrative statistics
report root, child, physical, and orphaned record counts separately.

Persist internal metadata for record scope, parent family identity, child ID,
and optional child ordinal. Child IDs are unique within
`(parent family identity, relation, child ID)`. Use a collision-free internal
encoding for overwrite/delete keys; do not concatenate user strings with an
unescaped delimiter or expose these keys as public IDs. Reject `allow_dups`
for relation-managed records.

A parent's external `id` is not sufficient as its lifetime identity. Assign a
persisted family incarnation when creating a root. Parent metadata replacement
preserves it. Parent deletion closes it; recreating the same external ID creates
a new incarnation. This prevents old children from silently attaching to the
new parent. An explicit full-family replacement publishes a new incarnation
and its complete children atomically.

The writer must resolve child mutations against ordered admission state,
including earlier admitted but uncommitted parent mutations. Extend the
versioned writer metadata and rollback protocol; do not resolve against an
arbitrary search reader. Support an expected-family token on child mutations
so delayed writes can fail instead of updating a recreated family. Reject a
child mutation with no valid parent. Persist incarnation allocation and closure
state in the commit/replication lifecycle and handle crash recovery explicitly.

Serialize family metadata transitions by external parent ID and update order,
while permitting independent families to invert in parallel. A child operation
depending on a pending parent creation/replacement must bind after that
operation succeeds, or fail if its prerequisite fails. Rollback cannot leave a
later operation referring to an incarnation that never became valid. This
dependency handling needs its own concurrent-write tests.

This is more work than a string foreign key, but makes deletion, replacement,
and concurrent writes dependable. It is part of the first implementation,
not cleanup to be added after the search API ships.

### Mutation operations

Define distinct operations instead of overloading an ambiguous partial update:

| Operation | Semantics |
| --- | --- |
| Parent upsert | Changes parent fields; preserves children and family incarnation for an existing parent. |
| Child upsert/delete | Changes one explicitly addressed child in the active family. |
| Full-family replace | Replaces parent and all children as one logical atomic operation; removed children cannot remain visible. |
| Parent delete | Immediately hides the parent and all its children in the next published snapshot; physical reclamation may follow later. |

Offer nested JSON as ingestion convenience over these operations. Example:

```json
{
  "families": [{
    "mode": "replace",
    "parent": {"id": "p1", "title_t": "Trail shoe", "brand_s": "Acme"},
    "children": {
      "variants": [
        {"id": "sku-red-42", "color_s": "red", "size_i": 42,
         "price_i": 9900, "stock_i": 8,
         "description_t": "Waterproof trail shoe"}
      ]
    }
  }],
  "commit": {}
}
```

Atomic family acceptance is separate from request-level `all_or_none` across
multiple families. A malformed child rolls back its entire family. Bound
family bytes and child count before admission. Reject oversized families with
a useful error; do not split them into independently visible updates. Large
streamed staged-family transactions are a later feature.

### Relation reader

Introduce an immutable `RelationReader` owned by the `IndexReader` snapshot:

- Immutable segment columns/dictionaries describe child-to-family membership.
- A reader-scoped directory maps a live family incarnation to its unique live
  parent `segdoc` and supplies relation membership/liveness.
- Parent-to-child access uses family-key postings, batched across requested
  parents. A compact adjacency structure is optional where benchmarks justify
  it; do not duplicate the entire relation in a hash table by default.
- Candidate resolution may use a sparse lookup cache; dense ordinal-to-parent
  tables are a budgeted acceleration, selected for suitable cardinalities.

Start with existing indexed columns and `OrdMap` where applicable, but isolate
identity resolution behind this reader. Share immutable segment parts across
refreshes. Account for directory construction and retained memory, including
coexisting old readers. Relation warmup must not introduce an unreported
unbounded cost into every commit or first query.

Prefer fixed-width engine-assigned family identities in the hot path; external
parent ID strings are an admission/projection concern. A dense 64-bit parent
locator per child alone costs 80 MB for 10 million children, before columns,
directories, and concurrent snapshots. This is a sizing example, not a measured
requirement: compressed segment membership and sparse resolution can avoid
that allocation. Benchmark directory refresh cost separately from query cost.

Logical child liveness is physical liveness AND membership in an active family.
Apply that rule to every child query, facet, count, and vector eligibility
selector, including exact fallback and ANN work estimates. A physically live
orphan must not consume vector candidate capacity or appear through an
alternative child operation.

A mandatory relation column or incarnation record is correctness data. It
cannot use the current optional-ANN-overlay behavior of being silently absent
after a build failure. Optional accelerators may fail back to the canonical
column/postings path. Missing mandatory metadata fails opening the affected
collection rather than exposing incorrect relationships.

Keyed relations survive merge because they refer to stable family identities,
not persisted `segdoc`s. Rebuild reader-local locators after remapping. Drop
closed-family children during compaction only when the closure and admission
watermarks make that safe; retained snapshots keep their original files.

## Query and ranking semantics

Add a `has_child` query returning the parent scope, with a declared relation,
child query, child filters, score reducer, and optional named `inner_hits`.
`max` is the default reducer; `none` supports filter-only existence. Implement
exact `sum`, `avg`, and min/max child-count constraints after the max/none path
is validated. Do not accept unsupported combinations and silently approximate
them.

For eligible children C(p), the default score is:

```text
parent_matches(p) = parent_eligible(p) AND exists c in C(p): child_matches(c)
parent_score(p)   = max { child_score(c) : c in C(p), child_matches(c) }
witness(p)        = argmax of the same set, with a documented tie-break
```

Use deterministic ties within the snapshot; an explicit child-ID tie sort
provides stable display across merges where needed. Parent relevance may be
combined with this score using the existing Boolean/boost/rescore machinery.

Proposed eCommerce request:

```json
{
  "filter": ["brand_s:Acme"],
  "query": {
    "has_child": {
      "relation": "variants",
      "query": "description_t:waterproof",
      "filter": ["color_s:red", "size_i:42", "price_i:<=10000", "stock_i:>0"],
      "score_mode": "max",
      "inner_hits": {
        "name": "matching_variants",
        "limit": 3,
        "fields": ["id", "price_i", "color_s", "size_i"]
      }
    }
  },
  "fields": ["id", "title_t", "brand_s"],
  "limit": 20
}
```

The variant must satisfy every child condition. In contrast,
`has_child(A) AND has_child(B)` may be witnessed by different children. Also,
`NOT has_child(red)` means no red child, whereas `has_child(NOT red)` means
there is a non-red child. These distinctions need explicit parser tests and
documentation. Define negation over the live parent scope, including parents
with zero children.

Root fields and child fields need distinct physical namespaces so their text
statistics and schema bindings do not mix. Luxir's BM25 already uses
`docsWithField`; retain field-local scoring instead of introducing an unrelated
global root/child document denominator.

### Query execution

Introduce a scoped document domain, carrying the relation/root scope with its
per-segment sets. A parent domain cannot be intersected directly with child
hits even if both use the same physical segment ID space. Translate it through
the relation first.

```mermaid
flowchart LR
  P[Eligible parent domain] --> R[Translate through relation]
  C[Child query and filters] --> E[Eligible child search]
  R --> E
  E --> A[Aggregate by live parent]
  A --> T[Rank and page parents]
  T --> H[Fetch parent fields and match evidence]
```

Implement a conservative prepared join first:

1. Pin the relation reader and establish eligible parents.
2. Translate parent eligibility to child membership, intersecting child scope,
   child liveness, and mandatory child filters.
3. Execute child matching over all required segments and accumulate by live
   parent identity. Use sparse accumulators for selective results and budgeted
   dense arrays only when profitable.
4. Produce immutable parent scores/membership bucketed by the parent's segment
   and sorted by parent doc ID. Replay those through the ordinary parent
   scorer, collector, and Boolean pipeline.
5. Retain compact witness references; project fields only for the final page.

Arbitrary keyed child order is not parent order. A streaming scorer that merely
substitutes child IDs with parent IDs violates iterator ordering and can miss
cross-segment matches. Whole-reader preparation avoids that error.

Add two physical strategies after the reference path works: search selective
children and aggregate parents, or start from a selective parent set and use
batched child postings. `explain` should identify the chosen strategy and
estimated/actual work. Both must produce the same exact lexical result.

Do not push arbitrary sibling queries into a bounded kNN node. Push only
mandatory eligibility proven safe by Boolean structure. Optional scoring
clauses remain optional; OR branches do not become mutual filters. For a
required lexical child condition plus vector retrieval, prepare its membership
before selecting nearest parents. Reject unsupported cyclic dependencies
between bounded selections rather than inventing a fixed-point interpretation.

### Vector search

Inside `has_child`, expose `knn.unit: parents`; the relation resolves its
identity without requiring an outer collapse. Reuse the existing group-aware
heap, per-key approximate candidate allocation, mixed flat/IVF merging, and
full-precision refinement. Extract the small grouping-identity dependency
from `CollapsePlan`, preserving its optimized decoding paths.

Proposed chunk retrieval request, with a three-dimensional toy embedding:

```json
{
  "filter": ["status_s:published"],
  "query": {
    "has_child": {
      "relation": "chunks",
      "filter": ["language_s:en"],
      "query": {
        "knn": {
          "field": "embedding_v", "query": [0.2, 0.4, 0.6],
          "k": 100, "unit": "parents", "exact": false
        }
      },
      "inner_hits": {
        "name": "semantic", "limit": 1,
        "fields": ["id", "text_t", "start_i", "end_i"]
      }
    }
  },
  "fields": ["id", "title_t", "url_s"],
  "limit": 10
}
```

Here the vector node selects up to 100 eligible documents and the list returns
10 of them. The published-parent and English-chunk conditions constrain
selection, not just the returned page.

The important budgets are distinct:

- `k`: number of eligible parents selected by the vector node.
- `refine_candidates`: number of child candidates available for refinement.
- Per-parent candidate retention: several children may survive approximate
  admission so refinement can recover a different winning child.
- `inner_hits.limit`: requested evidence rows; not an ANN search-effort knob.

Track the vector value ordinal as well as child identity when a child has
multiple embeddings. Exact search returns exact best eligible children and
parents for the vector score. ANN reports both parent-selection and witness
approximation; returning k distinct parents does not certify either recall.
Preserve underfill/exhaustion warnings and make their unit parents.

Selecting k children and subsequently joining is never the implementation of
k parents. Neither is querying all children and dropping parents afterward.

### Hybrid search

Support two explicit aggregation orders. They answer different questions:

```text
Parent evidence fusion:
  lexical child search -> best lexical score per parent -> parent ranking
  vector child search  -> best vector score per parent  -> parent ranking
  fuse the parent rankings -> return parents with each source's witnesses

Same-child scoring:
  score each eligible child with all requested signals
  -> take the best combined child per parent
  -> return parents and those children
```

For document retrieval, parent-level RRF is the recommended initial hybrid
mode. Each source has a bounded list of unique parents, and source `limit`
counts parents. Sources returning different scopes cannot be fused without
explicit projection. The existing fusion `segdoc` key is sufficient once both
sources return actual parents; do not create a second competing parent-ID
system inside fusion.

For example, source A may retrieve chunk 2 of parent P and source B chunk 9
of P. Parent fusion gives P both contributions. Chunk fusion followed by
collapse generally does not. Repeated chunks from one parent must not earn
repeated RRF contributions from the same source.

The proposed API composes existing fusion with the new query:

```json
{
  "ops": {
    "results": {
      "fusion": {
        "sources": {
          "lexical": {
            "query": {"has_child": {
              "relation": "chunks", "query": "text_t:refund",
              "inner_hits": {"name": "lexical", "limit": 1}
            }},
            "limit": 100
          },
          "semantic": {
            "query": {"has_child": {
              "relation": "chunks",
              "query": {"knn": {
                "field": "embedding_v", "query": [0.2, 0.4, 0.6],
                "k": 100, "unit": "parents"
              }},
              "inner_hits": {"name": "semantic", "limit": 1}
            }},
            "limit": 100
          }
        },
        "rrf": {}, "limit": 10
      }
    }
  }
}
```

`inner_hits` here is evidence requested by a query, not a per-source search
operation. Its transport through fusion is new work; today's source-op
restrictions do not provide it automatically.

For same-child hybrid relevance, calculate the complete child score before
parent max. One lexical and one vector witness are insufficient: a third child
may have the highest combined score without being best in either signal.
Use an explicit child-candidate union and rerank every retained candidate with
both signals. A `rescore` path can handle a single child candidate source;
multi-source child candidate union needs a dedicated preparation step, not
embedding a `Fusion` search op into `Query` without a defined contract.

Expose candidate-window semantics honestly. Exact vector similarity rescoring
does not make ANN parent recall exact. Exact scores over a finite hybrid union
do not prove global top-k for a different combined scoring function. Parent
RRF over declared source windows is fully defined, but is not fusion over
unretrieved ranks. Any future exact combined top-k path needs exhaustion or
valid score bounds with proven stopping conditions.

### Returning the actual match

Add a page-stage `MatchEvidence` interface keyed by named query/source and
actual parent `segdoc`. Evidence includes relation, public child ID, optional
ordinal/offsets, child score, and optional vector value ordinal. The parent
score remains separate from each evidence score.

For max scoring, preserve the selected witness even if a different child is
chosen for display sorting. For lexical inner hits, gather top matching
children on the returned page by replaying the same query semantics against
the same reader. Batch lookups across page parents, following the existing
`CollapseMembers` approach; avoid one independently prepared query per parent.

For bounded vector queries, default inner hits are admitted witnesses from the
original prepared selection. A later per-parent vector search is a different
query and must not silently replace them. Returning additional locally nearest
chunks is an explicitly named related-record operation, with its own scores
and completeness. If exact per-parent refinement is requested for ranking,
perform it for the complete candidate-parent set before final sorting and
retain the new ranking witnesses.

For fusion, return evidence per source and optionally a deduplicated union.
Do not invent a single chunk whose score equals a parent's fused score when
different chunks contributed. A future passage reranker can choose a display
chunk, but should retain the retrieval evidence for explanation.

Preserve both row and column result formats. Use a typed, parent-row-aligned
inner-hit structure in `DocList`, rendered as a reserved `_matches_` member
for JSON rows. Evidence is optional; a request that only asks for parents
should not allocate child payloads. Complete each parent's evidence before
emitting its streamed row. Highlighting can consume the original scoped query
later; chunk text and producer-supplied offsets are sufficient for v1.

Illustrative row for a max-vector parent result:

```json
{
  "id": "document-17",
  "title_t": "Returns policy",
  "_score_": 0.91,
  "_matches_": {
    "semantic": {
      "relation": "chunks",
      "scope": "admitted_witnesses",
      "docs": [{
        "id": "chunk-9", "_score_": 0.91,
        "text_t": "Refunds are available within thirty days.",
        "start_i": 420, "end_i": 465
      }]
    }
  }
}
```

The score and offsets are illustrative. ANN quality/completeness metadata
belongs with the selection result; the child list above makes no claim to
enumerate every matching chunk. Hybrid results may contain both `lexical` and
`semantic` evidence with different children and a separate parent fusion score.

## Sorting and analytics

eCommerce needs more than ranking by a child's text score. Implement a typed
parent-sort reduction over the same eligible child domain: for example minimum
price among matching in-stock variants. Keep the reduced sort value and its
attaining child separate from the max-text-score witness. A cheaper nonmatching
variant must not set the price of a filtered result unless explicitly requested.

Define domains for analytics rather than inferring them from nesting:

| Domain | Meaning |
| --- | --- |
| Parents | Distinct matched parents; default for top-level parent facets and counts. |
| Matching children | Children satisfying the named child query and its eligibility. |
| Witness children | Children admitted by the named bounded selection or retained as ranking evidence. |
| All eligible children | Siblings reached through the relation after an explicit domain reset. |

For a color facet, expose whether counts are variants or distinct parents.
Product counts require distinct parent IDs per bucket. Reuse the existing
distinct-key facet machinery with the relation's parent identity.

Keep full lexical match analytics distinct from facets over vector/hybrid
candidate windows. Never label an ANN witness count as the count of all
semantically relevant chunks. `found`, `found_gte`, and inner-hit totals must
declare the counted domain and omit exact totals when they were not computed.

Domain resets may replace relevance/filter clauses, but must retain structural
relation scope and mandatory caller eligibility. Existing Luxir domain resets
can drop ordinary filters; callers must opt into preserving business filters
when that is intended. Any future access-control predicate belongs in a
non-droppable eligibility layer. Do not suggest that Luxir currently has an
authorization system beyond its documented boundary.

## Correctness and resource contracts

- **Cache identity:** include relation schema, scope, pinned reader/family
  liveness, reducer, and relevant domain dependence. Cross-segment join outputs
  are reader-dependent; child segment stability alone does not make them
  core-cacheable. Keep arbitrary prepared domains request-local initially.
- **Witness demand:** presentation fields should not change membership cache
  identity. Evidence capacity/completeness must be tracked separately so a
  cached top-1 witness cannot satisfy a request for top-3 matching children.
- **Pruning:** start conservative. A parent max threshold may bound child max
  work, but sum/average/count predicates, two-phase verification, and mixed
  parent/child scoring require distinct proofs. Never advertise dense match-all
  or exact-count capabilities merely because the child query has them.
- **Memory:** account for relation directories, translated domains, parent
  accumulators, retained candidates, evidence, old snapshots, and parallel
  worker state. Exceeding an exact query's budget returns a resource error;
  it must not quietly truncate its parents. ANN effort caps remain explicit.
- **Cancellation:** check during relation construction, accumulation, candidate
  refinement, and evidence fetch. Join worker tasks before releasing arenas.
- **Consistency:** parent lookup, matching, fusion, metrics, and projection use
  one reader. Persist all required relation files in manifests so replication,
  checksums, retirement, and recovery cover them.
- **Routing:** keep one family on one shard if distributed writes/search are
  added. This proposal does not assume that distributed execution exists today.
- **Schema:** child scopes participate in signatures, schema generations,
  parser validation, and no-backfill rules. Old flat records remain root
  records; do not infer relations from coincidental field names.

## Implementation sequence

These are dependency-ordered milestones, not calendar estimates. Each milestone
should be split into reviewable PRs. Land no production query API until its
storage and lifecycle correctness prerequisites pass.

| Milestone | Main changes | Exit criterion |
| --- | --- | --- |
| 0. Contract and benchmark harness | Small exhaustive parent/child oracle; adversarial datasets; explicit scope, score, mutation, and result contracts; keyed-vs-block prototype measurements. | Exact expected results cover both use cases and distinguish all aggregation orders. Establish baseline latency, memory, and update amplification. |
| 1. Schema, identity, and writes | `protos/luxir_types.proto`, handwritten API types/JSON, `Schema`, `ProtoUpdateMessage`, `Inverter`, `IdHandler`, `IndexWriter`, persisted family metadata. | Root/child isolation; atomic family replace; independent child updates; concurrent delete/recreate and recovery tests pass. |
| 2. Relation readers | New `RelationReader`; `IndexReader`/`PostingsReader` integration; postings lookup; live-parent directory; merge, replication, memory accounting. | A child resolves correctly across segments and snapshots, and never resolves to a closed or different family incarnation. |
| 3. Exact parent queries and evidence | New `HasChildQuery`/prepared weight; scope-aware parser and domains; conservative `QueryShape` and cache behavior; page evidence through `EmitDocs`. | Text, numeric and attribute queries return real parents and correct witnesses; parent Boolean composition and negation match the oracle. |
| 4. Parent vector retrieval | Generalize grouping identity in `KnnQuery`; `unit: parents`; prefiltered parent eligibility; vector witness identity and warnings. | Exact results equal exhaustive parent search; ANN parent and witness recall measured separately; mixed IVF/flat and adversarial fanout work. |
| 5. Parent fusion | Lift source queries before `FusionOp`; validate common parent scope; preserve evidence by source; parent-sized source windows. | Different lexical/vector children contribute to one parent; duplicate chunks never multiply one source's contribution. |
| 6. eCommerce analytics and child reranking | Matching-child price sorts; relation domain transitions; parent-distinct child facets; multiple inner hits; same-child hybrid candidate union and rerank. | Sorts/facets use the intended variants; hybrid tests include a winner that is not either signal's individual winner. |
| 7. Performance and operational release | Adaptive parent-led/child-led plans; proven max-score pruning; refresh profiling; budget enforcement; docs and protocol reference. | Correctness, ASan, replica/recovery, recall and latency gates pass at target scale. Both use cases are documented end to end. |
| 8. Optional block backend | Physical family blocks and metadata behind the same relation/query/evidence interfaces. | Material measured benefit for replacement-heavy workloads justifies storage and merge complexity; behavior matches keyed backend. |

Milestones 1-5 provide parent retrieval for lexical, vector, and parent-level
hybrid search. The requested full product experience also needs milestone 6;
do not mark the overall feature complete at the first working join.

Register new query kinds consistently in `QueryKind`, `QueryShape`, filter-key
encoding, parser visits, expression functions, handwritten protobuf metadata,
and documentation generation. Use `arenaCreate` for objects requiring
destruction. The adjacent plugin extension proposal is not an implemented
dependency; these are core query and storage semantics.

### Optional block backend constraints

If measurements justify it, add an explicit storage policy for families
replaced together. Store children followed by the root in one segment, with
root boundaries and relation membership metadata. Admission, flush, merge,
delete, and any future index sorting must preserve whole-family boundaries.
Do not derive boundaries solely from live roots: deleted parents must not
cause their children to be attributed to the next root.

Independent child updates in block mode must have a documented behavior,
initially rewriting the complete family, rather than quietly breaking the
adjacency invariant. Preserve canonical keyed metadata so query behavior and
evidence remain consistent. A segment containing both policies needs an
explicit dispatch plan; never guess layout from observed doc-ID locality.

## Validation and performance gates

Use real RAMDir/FSDirectory collections and the existing CollectionHelper and
LocalReq utilities. No mocks. Compare optimized execution against an exhaustive
oracle that joins live records by family identity, evaluates each child, then
aggregates parents according to the requested semantics.

Required adversarial cases:

1. Red-small and blue-large variants must not match red-large within one child.
2. Separate `has_child` clauses may use different children; negation handles
   empty families and differs from a negated child predicate.
3. One parent owns the nearest 100,000 chunks; retrieval still selects k
   distinct eligible parents when enough exist.
4. The best vector child fails price, stock, language, or parent eligibility;
   select the best remaining eligible child before the parent cut.
5. Text and vector winners belong to different children of one parent;
   parent fusion combines both, and same-child fusion does not fabricate a
   child containing both scores.
6. A third child wins combined relevance while being second-best in each
   individual signal; premature per-source top-1 child retention must fail
   the oracle.
7. Parent/children span multiple segments; merges, deletes, parent recreation,
   and out-of-order completion preserve family identity and scores.
8. Phrase and other two-phase child queries cannot admit an unverified witness.
9. Parent filters, multiple relations, nested operations, and domain resets do
   not mix physical document scopes or reuse incompatible cache entries.
10. Atomic family rollback, crash/reopen, replica installation, cancellation,
    tiny budgets, and streamed responses never expose partial families or
    incomplete parent rows as complete results.

Benchmark both product and chunk distributions. Proposed scale points, to be
adjusted to available hardware, are 100K and 1M parents, with uniform and skewed
fanout ranging from 1 to 1,000 children and explicit giant-family cases. Vary
filter selectivity, relation cardinality, segments, hot/cold cache, vector
dimensions, approximate index type, and concurrent parent/child updates.
Include sparse and broad lexical queries, phrase verification, price sorting,
facets, and two-source hybrid queries. Add real relevance judgments for product
and document tasks; algorithmic recall alone does not establish usefulness.

Measure:

- Exact parent IDs, scores, witness IDs, sort values, and counts against the
  oracle, including serial/parallel and pre/post-merge parity.
- ANN ParentRecall@k, best-witness recall conditional on a recovered parent,
  score error, underfill, and hybrid relevance metrics such as nDCG@k.
- p50/p95/p99 latency, CPU, candidate/refinement work, evidence-fetch cost,
  resident bytes per parent/child, and peak memory with concurrent snapshots.
- Refresh/publication latency, cold relation initialization, ingest throughput,
  bytes written per child mutation, and merge/replication overhead.
- Regression on flat-only collections, including default queries and counts.

Exact correctness is a release gate. ANN quality is a measured tradeoff with
explicit effort limits. Set numerical latency/recall targets from the baseline
and actual product requirements; no external paper establishes Luxir's likely
speedup. Run the relevant native/container suites and ASan before committing
implementation work, following the checkout's AGENTS.md workflow.

## Scope after the first release

Keep the interfaces ready for deeper relation paths, learned parent reranking,
and optional block navigation. Defer arbitrary many-to-many joins,
cross-collection joins, cross-shard families, token-level late-interaction ANN,
and unbounded inner-hit pagination until each has a separate consistency,
cost, and scoring contract. These extensions should build on the same parent
identity and evidence model rather than bypassing it.


## Initial implementation measurements

Measured on 2026-10-27 with the native `gcc-release` preset on the development
machine (28 logical CPUs, CPU scaling enabled). These are short synthetic
smoke measurements with concurrent machine load, not production latency
estimates. Each search runs on one thread over a warm RAM-backed collection
with 4,000 parents in two segments, four-dimensional L2 vectors, and either
4 or 64 children per parent. Eligibility filters apply to both scopes; one
family concentrates nearby vectors to exercise distinct-parent selection.
The approximate cases use IVF-PQ. The benchmarks fail if approximate execution
does not visit IVF lists. Cold topology construction is outside timed search. Timed search includes
in-process request construction, JSON parsing, execution, and result projection,
but excludes HTTP/gRPC transport.

| Operation | 16,000 children | 256,000 children |
| --- | ---: | ---: |
| Exact grouped vector search | 0.310 ms | 7.444 ms |
| ANN adaptive probing | 0.194 ms | 2.895 ms |
| ANN all-list probing | 0.416 ms | 6.859 ms |
| Correlated child filter | 0.081 ms | 2.264 ms |

The mutation benchmark uses a separate single-family collection:

| Update operation | 4-child family | 64-child family |
| --- | ---: | ---: |
| Independent child replacement and RAM commit | 0.069 ms | 0.063 ms |
| Complete family replacement and RAM commit | 0.111 ms | 0.234 ms |

All eight retrieval cases achieved parent recall 1.0, witness recall 1.0, and
zero underfill against their oracle. This synthetic result does not establish
ANN recall on real embeddings. The four mutation cases distinguish one-record
updates from replacement of 5 or 65 physical records. Their RAM commits do not
measure durable filesystem or replicated acknowledgement latency. RSS counters
are process-wide and cannot be interpreted as incremental join memory.

Reproduce the full measurement and its smaller correctness-only mode with:

```bash
./build/gcc-release/bin/luxir_test --bench --benchmark_filter=BM_ParentChild --benchmark_min_time=0.2s --benchmark_out=/tmp/parent-child-benchmarks.json --benchmark_out_format=json
./build/gcc-release/bin/luxir_test --gtest_filter=Benchmarks.all --benchmark_filter=BM_ParentChild --gtest_brief=1 --gtest_print_time=0
```

The smaller mode passed all 12 scenarios with 48 parents. Dedicated relation
tests cover correlated predicates, score/count reducers, phrase verification,
parent and child eligibility before vector selection, independent exhaustive
vector oracles across segments, evidence through optional clauses and fusion,
streamed rows, request memory limits, family rollback, independent mutations,
parent deletion/recreation, merge, protobuf transport, schema persistence,
parent-only facet resets, and HTTP replication.


### Validation result

The debug, ASan, and release builds completed without compiler warnings.
The final serial native run passed all 3,185 tests across 232 suites, with one
separately disabled test. The release benchmark correctness mode passed, and
all 12 full benchmark scenarios completed successfully.

The final full ASan run passed 3,171 tests and skipped 12, but failed two
pre-existing `Storage/ReplicationMatrixTest.snapshotsBarriersAndRestarts`
parameters: `ram_fs` and `fs_fs`. Both initially expected one active replica
and received zero. An isolated ASan rerun also failed: `ram_fs` timed out
waiting for catch-up and `fs_fs` again observed zero active replicas. There
were no AddressSanitizer or LeakSanitizer findings. All new relation tests,
including request memory limits and relation replication, passed in that
full ASan run. The sanitizer suite is therefore not fully green; these
replication timing failures remain unresolved. Heavy host load was observed,
but it is not proof of the cause. No timeout increases were made to hide them.

An earlier concurrent native run also had two replication failures; the
serial full native rerun above passed them. Preserve the distinction between
feature coverage, sanitizer findings, and full-suite test outcomes when
reviewing this implementation.

The final pre-PR review changed only the `has_child` protobuf tag from 26 to
27 to avoid the plugin-query arm on `warp-0.2.0`. Debug, ASan, and release
rebuilds were warning-free. After that change, the focused native run passed
all 22 relation, protobuf, and relation-replication tests; release benchmark
correctness mode again passed all 12 scenarios. Full-suite counts above are
from before this tag-only change.

The focused ASan run passed its 21 relation/protobuf tests but timed out in
the new relation-replication test during initial follower catch-up, before
its first search. With `TMPDIR=/dev/shm`, using the filesystem backend on
tmpfs, that same ASan test passed. The two earlier replication barrier cases
also passed under that configuration. This supports investigating temporary-
disk latency, but does not resolve the disk-backed ASan failures or substitute
for a clean full-suite run. No test code or timeout was changed for this
comparison, and no sanitizer findings were reported.
