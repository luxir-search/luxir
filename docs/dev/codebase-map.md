# Codebase Map

Class-by-class map of Luxir internals, oriented toward contributors and
coding agents. For the engine design overview and the reasoning behind it,
see [../design/architecture.md](../design/architecture.md). For exact file
locations, browse `src/luxir/<area>/`.

## Core Components

1. **Server Layer** (`src/luxir/server/`)
   - `GRPCServer`: Manages gRPC services and thread pool
   - `HttpServer`: JSON/HTTP API (Boost.Beast), including NDJSON streaming ingest
   - `LuxirNode`: Central coordinator managing collections and services

2. **Search Engine** (`src/luxir/search/`)
   - `IndexReader` is for reading a whole index and contains a `PostingsReader` per index segment
   - `SearchEngine`: Coordinates query parsing, execution, and result collection
     - Implements parallel segment searching using TBB

3. **Segment Reading** (`src/luxir/reader/`)
   - `PostingsReader` reads a single segment and owns the open files for that segment
   - `FieldReader` finds metadata for a field in the segment (`SegFieldInfo`)
   - `TermsEnum` enumerates or finds indexed terms for a field found by `FieldReader`
   - `DocsEnum` returns the documents for a term found with the `TermsEnum`. Optionally returns term positions for each document.
   - `IntColReader` reads the doc-order numeric column (all numeric field
     classes store an order-preserving encoded int64)
   - `PointsReader` reads the optional 1-D sorted-leaf points index of a
     `RANGE`-indexed numeric field (value-sorted leaves + fence directory;
     exact ordinal counts). `BKDReader` reads the 2-D int32 BKD of a
     `GEO_POINT` field. Absence of either = `SegFieldInfo.pointsMetaOff == 0`.

4. **Query System** (`src/luxir/query/`)

   Query execution flows from an index-independent `Query` through a
   cross-segment `Weight`, then through a per-segment `ScorerSupplier` that
   resolves retained construction plans for the cursor, docs-only, bulk, or
   constant-count product the consumer will execute. The Overview in
   [`Query.h`](../../src/luxir/query/Query.h) is the authoritative hierarchy and
   planning-contract description.

   - Supports Term, Boolean, Phrase, and All queries
   - `NumericRangeQuery` executes through the points index when present
     (direct materialization or complement), else zone-map pruned or full
     column scans; `GeoBoxQuery` executes through the BKD. Both share the
     `PointsMaterialize` scorer/bitset primitives and keep a sparse
     two-phase column verify for small lead costs.
   - `ProtobufQueryParser` lowers the wire tree (`luxir::api::Query`) via `QueryBuilder`
     (the single place query-time analysis is applied); `ParseContext` carries the
     request pool, schema, warnings sink, and shared nesting budget
   - String parsers emit `api::Query` subtrees and lower through the same path:
     `SimpleQueryParser` (never-fails search-box input) and `ExprParser` (the rigorous
     `expr` query language; `Cursor` is its bounds-checked input, `ExprFunctions.h` the
     reflection-driven function-form registry)

5. **Indexing** (`src/luxir/index/`)
   - `IndexWriter`: Handles multi-threaded indexing with TBB flow graph pipeline.
     - Manages `Inverter` instances, flushing, merging, and commits.
   - `Inverter`: Low level single-threaded document processing for a single segment.
   - `PostingsWriter`: used by an Inverter on flush to write a new segment.
   - `PointsWriter` (1-D sorted leaves) and `BKDWriter` (2-D geo) build the
     optional points indexes at flush; `SegmentMerger` carries 1-D points
     forward by run-merging and rebuilds geo BKDs from the merged column.

6. **Vector Search** (`src/luxir/index/`, `src/luxir/reader/`)
   - `VectorReader`: reads column-stored vectors for exact flat KNN
   - `VectorIndexBuilder` / `VectorAuxReader`: per-segment FAISS IVF+PQ aux overlays for ANN
   - Reuses column storage for exact search, cosine raw-column normalization, and full-precision rescoring
   - See [../guide/vector-search.md](../guide/vector-search.md) for the user contract (query knobs, scoring, recall) and [../design/vector-search.md](../design/vector-search.md) for the overlay/build/query internals

7. **Storage** (`src/luxir/store/`)
   - `Directory`: abstract storage interface. Implementations: `RAMDir` (in-memory, used by tests),
     `FSDirectory` (on-disk, mmap reads), `CheckedDirectory` (validation wrapper)
   - `InputStream` / `OutputStream`: segment I/O primitives
   - `DirectoryFactory`: constructs directories (`RAMDirFactory`, `FSDirFactory`, `CheckedDirFactory`)
   - Note: segment readers/writers live in `reader/` (`PostingsReader`) and `index/` (`PostingsWriter`), not in `store/`.

## Data Organization

- **Library**: Top-level multi-tenancy container
- **Collection**: Logical document group with schema
- **Shard**: Physical collection partition, consists of a single **Index**
- **Segment**: Immutable index unit

## Request Flow

**Search**: gRPC/HTTP Request -> SearchEngine -> Query Parsing -> Weight Creation -> Per-Segment Plan Resolution and Execution -> Result Collection -> Response

**Indexing**: gRPC/HTTP Request -> IndexWriter -> Document Processing (Inverter) -> Segment Writing -> optional Commit

## Key Design Patterns

- TBB flow graph for asynchronous indexing pipeline
- Custom memory pools (`MemPool`) for efficient single-threaded allocation that can be rolled back
- For multi-thread safe arena allocation with destructor support, use protobuf's Arena
- Parallel processing with work-stealing
- Streaming APIs for large result sets

## Proto Files

Protocol buffer definitions are in `protos/`:
- `luxir.proto`: Main service definitions
- `luxir_types.proto`: Common type definitions

Generated files are output to the build directory (`build/<preset>/protos/*.pb.h`).
