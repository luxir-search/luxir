# Architecture

High-level map of Solux internals. For exact file locations, browse `src/solux/<area>/`.

## Core Components

1. **Server Layer** (`src/solux/server/`)
   - `GRPCServer`: Manages gRPC services and thread pool
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

5. **Indexing** (`src/solux/index/`)
   - `IndexWriter`: Handles multi-threaded indexing with TBB flow graph pipeline.
     - Manages `Inverter` instances, flushing, merging, and commits.
   - `Inverter`: Low level single-threaded document processing for a single segment.
   - `PostingsWriter`: used by an Inverter on flush to write a new segment.

6. **Vector Search** (`src/solux/index/`, `src/solux/reader/`)
   - `VectorReader`: reads column-stored vectors for exact flat KNN
   - `VectorIndexBuilder` / `VectorAuxReader`: FAISS IVF+PQ aux indexes for ANN, with a FAISS-flat hook for tests and benchmarks
   - Reuses column storage for exact search, cosine raw-column normalization, and full-precision rescoring
   - See [vector-search.md](vector-search.md) for indexing, query knobs, and recall semantics

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

**Search**: gRPC Request -> SearchEngine -> Query Parsing -> Parallel Segment Search -> Result Collection -> gRPC Response

**Indexing**: gRPC Request -> IndexWriter -> Document Processing (Inverter) -> Segment Writing -> optional Commit

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
