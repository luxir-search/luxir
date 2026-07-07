# Codebase Map

Class-by-class map of Solux internals, oriented toward contributors and
coding agents. For the engine design overview and the reasoning behind it,
see [../design/architecture.md](../design/architecture.md). For exact file
locations, browse `src/solux/<area>/`.

## Core Components

1. **Server Layer** (`src/solux/server/`)
   - `GRPCServer`: Manages gRPC services and thread pool
   - `HttpServer`: JSON/HTTP API (Boost.Beast), including NDJSON streaming ingest
   - `SoluxNode`: Central coordinator managing collections and services

2. **Search Engine** (`src/solux/search/`)
   - `IndexReader` is for reading a whole index and contains a `PostingsReader` per index segment
   - `SearchEngine`: Coordinates query parsing, execution, and result collection
     - Implements parallel segment searching using TBB

3. **Segment Reading** (`src/solux/reader/`)
   - `PostingsReader` reads a single segment and owns the open files for that segment
   - `FieldReader` finds metadata for a field in the segment (`SegFieldInfo`)
   - `TermsEnum` enumerates or finds indexed terms for a field found by `FieldReader`
   - `DocsEnum` returns the documents for a term found with the `TermsEnum`. Optionally returns term positions for each document.

4. **Query System** (`src/solux/query/`)
   - `Query`: Abstract query representation
   - `Weight`: Query adapted to specific index
   - `Scorer`: Executes query on specific segment
   - Supports Term, Boolean, Phrase, and All queries
   - `ProtobufQueryParser` lowers the wire tree (`solux::api::Query`) via `QueryBuilder`
     (the single place query-time analysis is applied); `ParseContext` carries the
     request pool, schema, warnings sink, and shared nesting budget
   - String parsers emit `api::Query` subtrees and lower through the same path:
     `SimpleQueryParser` (never-fails search-box input) and `ExprParser` (the rigorous
     `expr` query language; `Cursor` is its bounds-checked input, `ExprFunctions.h` the
     reflection-driven function-form registry)

5. **Indexing** (`src/solux/index/`)
   - `IndexWriter`: Handles multi-threaded indexing with TBB flow graph pipeline.
     - Manages `Inverter` instances, flushing, merging, and commits.
   - `Inverter`: Low level single-threaded document processing for a single segment.
   - `PostingsWriter`: used by an Inverter on flush to write a new segment.

6. **Vector Search** (`src/solux/index/`, `src/solux/reader/`)
   - `VectorReader`: reads column-stored vectors for exact flat KNN
   - `VectorIndexBuilder` / `VectorAuxReader`: per-segment FAISS IVF+PQ aux overlays for ANN
   - Reuses column storage for exact search, cosine raw-column normalization, and full-precision rescoring
   - See [../guide/vector-search.md](../guide/vector-search.md) for the user contract (query knobs, scoring, recall) and [../design/vector-search.md](../design/vector-search.md) for the overlay/build/query internals

7. **Storage** (`src/solux/store/`)
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

**Search**: gRPC/HTTP Request -> SearchEngine -> Query Parsing -> Parallel Segment Search -> Result Collection -> Response

**Indexing**: gRPC/HTTP Request -> IndexWriter -> Document Processing (Inverter) -> Segment Writing -> optional Commit

## Key Design Patterns

- TBB flow graph for asynchronous indexing pipeline
- Custom memory pools (`MemPool`) for efficient single-threaded allocation that can be rolled back
- For multi-thread safe arena allocation with destructor support, use protobuf's Arena
- Parallel processing with work-stealing
- Streaming APIs for large result sets

## Proto Files

Protocol buffer definitions are in `protos/`:
- `solux.proto`: Main service definitions
- `solux_types.proto`: Common type definitions

Generated files are output to the build directory (`build/<preset>/protos/*.pb.h`).
