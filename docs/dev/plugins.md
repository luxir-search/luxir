# Plugins

Luxir extends through compiled-in plugins. There is no dynamic loading, no shared
objects and no C ABI: a plugin is a CMake target that the engine executables link,
and it registers its components through a generated translation unit.

## Model

- `luxir_lib` never depends on plugins. Plugins depend on `luxir_lib` and use its
  headers directly.
- The `luxir` executable links the library `luxir_plugins`, which holds the
  generated `addConfiguredPlugins()` plus every non-test-only plugin library.
  `luxir_test` links `luxir_test_plugins`, which adds the test-only plugins.
- Registries (`luxir::Registries::global()`) are process-global. They are filled once
  by `luxir::initPlugins()` and then frozen: later registration throws, and lookups
  read immutable tables without locking.
- Registering a name that already exists throws `std::invalid_argument` during
  startup, so a misconfigured binary fails loudly instead of overriding silently.

## Startup order

`initPlugins(addPlugins)` runs after configuration is applied and before any `LuxirNode` or
`Schema` exists (`luxir_main` and the test `main` both call it). It is idempotent.

1. Built-in components are registered (`registerBuiltinAnalysis`, so built-in names are
   taken before any plugin can claim them).
2. The `addPlugins` callback (executables pass `addConfiguredPlugins`, so the reference
   comes from the executable and the generated object is linked in) adds every
   configured plugin, in link-list order.
3. Each plugin's `registerComponents(Registries&)` runs. A throw aborts startup.
4. The list is logged at info level (`plugins: name version, ...`).
5. The registries are frozen.

## Writing a plugin

A plugin directory holds a `CMakeLists.txt` and sources:

```cmake
luxir_add_plugin(NAME my_plugin
                 SOURCES MyPlugin.cpp
                 [LIBRARIES some::target]
                 [INCLUDE_DIRS include]
                 [TEST_SOURCES MyPluginTest.cpp]   # gtest files compiled into luxir_test
                 [TEST_ONLY])                      # link into luxir_test only
```

```cpp
#include "luxir/plugin/Registries.h"

class MyPlugin : public luxir::Plugin {
public:
  luxir::PluginInfo info() const override { return {"my_plugin", "1.0", "what it does"}; }
  void registerComponents(luxir::Registries& r) override { /* register into r */ }
};

LUXIR_PLUGIN(my_plugin, MyPlugin)
```

The `NAME` must match `[a-z0-9_]+` and the first argument of `LUXIR_PLUGIN`. The
macro defines `luxir_plugin_create_<name>()` in the global namespace, which the
generated translation unit calls.

## Building with plugins

Point `LUXIR_PLUGIN_DIRS` at one or more plugin directories (semicolon separated):

```bash
cmake --preset gcc-debug -DLUXIR_PLUGIN_DIRS="/path/to/a;/path/to/b"
cmake --build --preset gcc-debug
```

Each directory is added with `add_subdirectory`, under `build/<preset>/plugins/<basename>`.
The configure step prints the configured plugin names, and the server logs them at
startup. The generated sources live in `build/<preset>/generated/`.

Plugin targets see the include directories `luxir_lib` keeps private (generated
protos, hash maps) and link TBB, so a plugin can build on engine internals such as
search ops, readers and `QueryPrep`.

## Node lifecycle

`Plugin::nodeStarted(LuxirNode&)` runs at the end of a node's construction, after
its collections are open; `nodeStopping(LuxirNode&)` runs first in its destructor,
in reverse plugin order. A process can run several nodes (tests do), so a plugin
that keeps per-node state keys it by node. A throw from `nodeStarted` aborts the
node's construction; `nodeStopping` is `noexcept`.

## Query types

`Registries::query` (`src/luxir/query/QueryRegistry.h`) adds query types next to the
built-in `Query` arms. On the wire a plugin query is the `plugin` arm,
`{name, params}` with `params` a map of typed JSON values. The JSON dialect also
reads a registered name as the arm key, so these are the same request:

```json
{"plugin": {"name": "doc_ids", "params": {"values": ["a", "b"]}}}
{"doc_ids": {"values": ["a", "b"]}}
```

An unregistered key stays an `unknown_key` JSON error; an unregistered `plugin`
name is a request error listing the registered types. Writes are canonical (the
`plugin` object), so `?explain=request` echoes that form.

```cpp
r.query.add("doc_ids", luxir::PluginQueryDef{
    [](luxir::PluginQueryContext& ctx) -> luxir::Query* {
      auto values = ctx.params.getStringList("values");
      const luxir::api::Val* within = ctx.params.get("within");
      ctx.params.rejectUnknown();
      // ... build an engine Query in ctx.parse.pool; nested queries go
      // through ctx.parseQuery(*within) ...
    },
    {{"values", luxir::PluginQueryArg::Kind::STRING_LIST},
     {"within", luxir::PluginQueryArg::Kind::QUERY}},
    "values"});
```

- The maker returns an engine `Query` allocated in `ctx.parse.pool` (queries are
  trivially destructible). It may return a built-in query (lower to `any_of`,
  boolean, ...) or its own `Query` subclass constructed with `QueryKind::PLUGIN`,
  which every planner shape question answers conservatively. All plugin query
  classes share that kind, so `equalsSameKind` must `dynamic_cast` the other
  query. A subclass that depends on documents across segments returns a
  reader-wide filter key scope (`READER_STABLE`).
- `ctx.parse.fields.note(text)` adds a line to `?explain=resolved`, for
  example what the query resolved its parameters to.
- `ctx.parse` is the request's `ParseContext`: pool, schema, the searched reader
  (`ctx.parse.reader`, null outside a search), coercion context, warnings and field
  resolver, which is everything `QueryBuilder` needs.
- `ctx.parseQuery(...)` lowers a nested `api::Query`, or a Query-shaped param (an
  object or an expression string), through the ordinary parser with the shared
  nesting budget.
- `ctx.params` (`PluginParams`) reads typed params and `rejectUnknown()` refuses the
  rest; every error names the type and the parameter.
- The expression arguments make the type callable in the expression language,
  `doc_ids([a, b], within=cat_s:x)`. Each argument parses by its kind (`STRING`,
  `NUMBER`, `BOOL`, `STRING_LIST`, `QUERY`); a `QUERY` argument is a sub-expression
  and reaches the maker as Query JSON. The positional argument, when named, takes
  the one positional value. Without expression arguments the type is JSON only.
- Names are `[a-z0-9_]+` and may not be a built-in arm name.

## Search ops

`Registries::searchOps` (`src/luxir/search/SearchOpRegistry.h`) adds operations next
to `top_docs`, `fusion` and the facets. On the wire a plugin op is the `plugin` arm,
`{name, params, ops}`; the dialect reads `{"<name>": {...}}` with the object's
`ops` member as the nested ops and every other member as a param.

The maker receives a `PluginOpContext` while the request is parsed and returns a
`SearchOp` subclass allocated with `luxir::arenaCreate` in `ctx.req.arena`:

- `parseQuery(...)` lowers and validates a query against the op's `planning()`
  context; `weightContext()` is the `Query::Context` for `createWeight`, and
  `planning().getFilterUse(query)` its filter cache entry.
- `parseSource(name, val)` parses a TopDocs-shaped param as a source the op runs
  itself, as `FusionOp` runs its sources: set its `rankingSink` and `parent`,
  create its calculator under the op's own, dispatch it per segment and receive
  the merged collector.
- `addSubs(op)` parses the nested ops into `op.subOps`. They run over whatever
  domain the op hands their calculators (`calcAll` with one `DomainHandle` per
  segment; an empty set, never null, for a segment with no documents), and may not
  declare their own `domain`.

The op's calculator follows the `SearchOp::Calculator` contract (see
`src/luxir/search/ops/SearchOp.h` and `FusionOp.h`). Results are a `Val` in the
response: `emitDocsResponse` (`EmitDocs.h`) writes a `DocList` with returned
fields, and its `extraColumns` add computed per-document values beside them
(`ExtraColumn{name, value(i, out, mr)}`). Extra columns are always returned; a
`fields` selection may name them, and one made only of extra columns (such as
`["_rank"]`) returns just those, while an empty selection projects the default
field set. A free-form result is a `Map`. Nested op
results go in the `DocList`'s `ops` (route them in `getTargetForSub`). `format=docs`
accepts only `top_docs` and `fusion` ops.

## HTTP routes

`Registries::http` (`src/luxir/server/HttpRouteRegistry.h`) adds routes to the HTTP
server. A pattern lives under `/_plugins/<plugin>/...` (node level) or
`/collections/{collection}/<_endpoint>/...` (collection level); `{name}` segments
capture. Built-in routes always win, so collection patterns may not end in a
built-in endpoint name or use `_snapshot`. Patterns may not overlap: two patterns
with the same segment count must differ at some position where both are literals
(`add` throws otherwise). Plugin routes are not reachable under `/tenants/{t}/`;
they always act in the default tenant, which is what `HttpPluginRequest::tenant`
holds.

```cpp
r.http.add("POST", "/collections/{collection}/_sample",
           [](luxir::HttpPluginRequest& request, luxir::HttpPluginResponse& response) {
  response.searchCollection = *request.pathParam("collection");
  response.searchBody = R"({"ops":{"sample":{"sample_docs":)" + std::string(request.body) + "}}}";
});
```

- The handler runs on a task-arena worker, so it may block on engine work through
  `request.node`. It answers with `status`, `contentType` and `body`, or sets
  `searchBody` to have the server run that body as a search with default URL
  options and answer as `_search` would (streaming, errors). The request's URL
  parameters are not applied to that search; they reach the handler as
  `queryParams`, so a route that wants `count_limit` or the like folds it into
  the body itself.
- A thrown `ApiError` answers with its classification; other exceptions are 500.
- A path with routes but not for the request's method answers 405 with `Allow`.
  Routes added with `mutating = true` are refused on a read-only node.

## The example plugin

`test/plugins/example` is a test-only plugin (always configured, linked into
`luxir_test` only). It registers a `doc_ids` query type, a `sample_docs` search op
(an extra `_rank` column and nested ops over its documents), two HTTP routes and
lifecycle counters, exercised end to end by `PluginExtensionTest`; `PluginTest`
covers the framework.

## Analysis components

`Registries::analysis` (`src/luxir/analysis/AnalysisRegistry.h`) maps names to makers
for four kinds of component. A maker receives the authored `api::AnalyzerComponent` (its
`name` and typed `params`), validates the params, and returns an immutable factory that
creates one stage per chain. Makers throw `std::invalid_argument` with a message that
teaches (what was wrong, what is valid); the registry adds "unknown filter 'x'; valid
filters: ..." for unknown names.

| Registration | Maker returns | Stage |
|---|---|---|
| `addTokenizer(name, maker)` | `TokenizerFactory` | splits the value into tokens |
| `addFilter(name, maker)` | `TokenFilterFactory` | wraps the previous stage (`TokenFilter`); `prepare` may analyze params through the preceding stages |
| `addCharFilter(name, maker)` | `CharFilterFactory` | rewrites the value before tokenization (`CharFilter`) |
| `addAnalyzer(name, def)` | (a complete `api::AnalyzerDef`) | a named analyzer |

Rules:

- Names are `[a-z0-9_]+`, unique per kind. A duplicate, or any add after the freeze,
  throws. Built-ins cannot be replaced.
- Put expensive shared state (a word set, a dictionary) in the factory, which is shared by
  every field using the definition; stages are per chain and not thread-safe.
- Follow the token borrow contract in `Analyzer.h`: a filter never writes the bytes it was
  given, it copies into its own buffer and repoints `token.text`. Set `normalizing = true`
  and override `normalizeTerm` only if the transformation is safe to apply to a multiterm
  (prefix, fuzzy) literal; stemmers are not. Set `stateful = true` if the stage buffers
  across tokens and needs `reset()` per value.
- A char filter (`src/luxir/analysis/CharFilter.h`) overrides `filter()`, reading `input`
  and calling `replace(start, end, text)` for each rewrite in increasing input order;
  unchanged input is borrowed and the base class records the edits, so
  `TokenChain::sourceOffset` maps token offsets back to the original text. A
  `normalizing` char filter also runs on multiterm literals.
- `addAnalyzer` deep-copies the definition into registry-owned storage, so the
  `AnalyzerDef` and its arena views may die right after the call. A definition may not
  itself be a named analyzer. Its components are checked when the registries freeze, so
  they may be registered after the analyzer; one that does not resolve fails startup.
  `Analyzer::compile` stores the expanded definition, never the name, in the canonical
  JSON that segment metadata persists. The stored schema keeps the name and expands it
  again at every load, so changing a registered analyzer takes effect for a collection
  when it next loads, for queries and new documents alike.
- The fused `unicode_word` + `nfkc_cf` fast path applies only to exactly those built-in
  components; plugin stages always run as written.
- `Token` carries more than its text, offsets and `positionIncrement`. The whole chain shares
  one `Token`, so a filter that rewrites or drops tokens in place forwards every attribute;
  one that buffers tokens past the current pull saves and restores what it needs. See
  [Token attributes](#token-attributes).
- A collection whose persisted schema names a component this binary does not have fails to
  load with the "unknown ..." message; the node keeps serving its other collections.

The example plugin registers a `reverse` filter (reverses token bytes, `normalizing`), a
`strip_x` char filter (removes every `x` through `replace`) and the analyzer `example` =
`whitespace` + char filter `strip_x` + filters `lowercase`, `reverse`:

```cpp
void registerComponents(luxir::Registries& r) override {
  r.analysis.addFilter("reverse", [](const luxir::api::AnalyzerComponent& def) {
    struct Factory : luxir::TokenFilterFactory {
      std::unique_ptr<luxir::TokenStream> create(std::unique_ptr<luxir::TokenStream> source) const override {
        return std::make_unique<ReverseFilter>(std::move(source));
      }
    };
    auto factory = std::make_unique<Factory>();
    factory->name = "reverse";
    factory->normalizing = true;
    return std::unique_ptr<luxir::TokenFilterFactory>(std::move(factory));
  });
  // addCharFilter("strip_x", ...) is the same shape with a CharFilterFactory.

  luxir::api::AnalyzerComponent whitespace, stripX, lowercase, reverse;
  whitespace.name = "whitespace";
  // ... set the other names ...
  luxir::api::AnalyzerDef example;
  example.tokenizer = whitespace;
  const luxir::api::AnalyzerComponent charFilters[] = {stripX};
  const luxir::api::AnalyzerComponent filters[] = {lowercase, reverse};
  example.char_filters = charFilters;
  example.filters = filters;
  r.analysis.addAnalyzer("example", example);
}
```

A schema then uses the plugin by name (`"analyzer": "example"`) or composes its components
(`"analyzer": {"tokenizer": "whitespace", "filters": ["reverse"]}`).
Text is fed to a chain with `TokenChain::setValue`, never `head.setValue`, so char filters
always run.

### Token attributes

Besides `text`, `positionIncrement` and the offsets, a `Token` has three attributes, all
reset by `Token::clear()` when a tokenizer starts a token:

| Attribute | Default | Meaning |
|---|---|---|
| `positionLength` | 1 | Positions the token spans: its arc length in the token graph (Lucene's `PositionLengthAttribute`), 2 for a synonym covering two source words. `uint16_t`. |
| `type` | `UNKNOWN` | `TokenType`: `ALPHA` (letters or ideographs), `NUMERIC`, `ALNUM`, `OTHER`. |
| `script` | `UNKNOWN` | `Script` of the token's first letter: `LATIN`, `HAN`, `HIRAGANA`, `KATAKANA`, `HANGUL`, `THAI`, `ARABIC`, `CYRILLIC`, `GREEK`, `OTHER`; `COMMON` when it has no letter (digits). |

`unicode_word`, and so the fused `unicode_word` + `nfkc_cf` tokenizer, sets `type` and
`script` (the fused ASCII path from its byte classes, other words from their code points;
both give the same answer). `whitespace`, `keyword` and a plugin tokenizer leave them
`UNKNOWN` unless they set them; `scriptOf(char32_t)` is the per-code-point lookup. The
attributes are read before any filter rewrites the text, so `type` and `script` describe the
source word: a CJK bigram filter can branch on `token.script == Script::HAN` instead of
guessing from the first byte. The three sit next to `positionIncrement`, so `clear()`
resets all four with one 8-byte store (`Analyzer.h` has a `static_assert`).

`positionLength` is carried and nothing reads it yet. Postings store only each term's start
position (as Lucene's do), and phrase queries are built from position increments, so a
token spanning several positions indexes and matches exactly like a one-position token.
Honouring it needs a query-side graph (a phrase over alternative paths); a filter that
produces graph tokens can set it today and a later query builder can consume it.

`TokenAttributesExampleTest` shows a plugin filter (`describe`) reading all three, and the
example `span2` filter setting `positionLength`.

## Collations

`Registries::collation` (`src/luxir/collation/CollationRegistry.h`) maps names to makers of
collators: objects that turn a string into a binary sort key, so that comparing keys as
unsigned bytes orders the strings by a locale or custom rule. A maker receives the authored
`api::AnalyzerComponent` (`name` and typed `params`, for example a locale or strength),
validates the params, and returns an immutable `CollatorFactory`; `create()` makes a
`Collator` per use, which is not thread-safe.

```cpp
class MyCollator : public luxir::Collator {
public:
  void appendKey(std::string_view value, std::string& key) override { /* append key bytes */ }
};

void registerComponents(luxir::Registries& r) override {
  r.collation.add("my_collation", [](const luxir::api::AnalyzerComponent& def) {
    // validate def.params, throw std::invalid_argument("... valid values: ...") on a bad one
    struct Factory : luxir::CollatorFactory {
      std::unique_ptr<luxir::Collator> create() const override { return std::make_unique<MyCollator>(); }
    };
    return std::unique_ptr<const luxir::CollatorFactory>(std::make_unique<Factory>());
  });
}
```

Rules are the analysis ones: `[a-z0-9_]+` names, a duplicate or an add after the freeze
throws, and the registry adds "unknown collation 'x'; valid collations: ..." for unknown
names. There are no built-in collations.

A string field names a collation in its schema:

```json
{"fields": {"title": {
  "type": "string",
  "variants": {"sort": {"type": "string", "index": "none", "column": true,
                        "collation": {"name": "my_collation", "params": {"locale": "sv"}}}},
  "defaults": {"value": "sort"}}}}
```

At ingest the field's column stores the collator's key for each (normalized) value instead of
the value, so the existing string column sort orders by it and nothing else in the sort path
changes. Because the column then holds opaque bytes, the field must be column-only
(`index: "none"`, `column: true`; anything else is a schema error), which is why it is
normally a variant of the field it sorts: `defaults.value` makes sorts on `title` use it,
while search, retrieval and the `title` primary keep the text. Facets and value expressions
over the collated field see keys. Range queries on the primary field are untouched.

The collation is part of the field's identity. The authored definition is in the persisted
schema, and the canonical name and parameters are in the field signature, so changing a
collation (different params, or a plugin upgrade that changes keys under the same name) makes
the field a new representation: segments written before the change are not read as holding
the new keys, the same contract as other schema changes. A collection whose schema names a
collation this binary lacks fails to load with "unknown collation 'x'; valid collations: ...";
the node keeps serving its other collections.

The example plugin registers `example_ci` (ASCII case-insensitive order, `strength` of
`primary` or `tertiary`); `CollationExampleTest` covers sorting, params, signatures,
persistence and the missing-collation failure.

## Storage backends and layers

`Registries::storage` (`src/luxir/store/StorageRegistry.h`) holds named storage
backends and layers. `ram`, `fs` and `ram_object` are built in and registered before plugins.

- A **backend** builds the root `DirectoryFactory` from the node configuration. It
  is selected by `--store.backend NAME`.
- A **layer** wraps the factory built so far. Layers are selected by
  `--store.layer NAME` (repeatable, innermost first) and sit between the backend and
  the core wrappers: `checked_dir` and `read_only` stay outermost, so a mutation is
  refused before any layer sees it.

Both makers receive a `StorageContext`: the whole `LuxirConfig`, the free-form
`options` map (`--store.option KEY=VALUE`, read directly or through `option()`,
`optionOr()` and `optionBytes()`), and `reclaim`, the callback RAM backends call
under memory pressure. Unknown names fail when the node opens its storage, with a
message listing the registered names.

```cpp
void registerComponents(luxir::Registries& r) override {
  r.storage.addBackend("s3", [](const luxir::StorageContext& ctx) {
    return std::make_unique<S3DirFactory>(ctx.option("bucket").value_or(""));
  });
  r.storage.addLayer("cache", [](std::unique_ptr<luxir::DirectoryFactory> inner, const luxir::StorageContext& ctx) {
    return std::make_unique<CacheLayer>(std::move(inner), ctx.optionBytes("cache_bytes", 1ULL << 30));
  });
}
```

A backend implements `DirectoryFactory`. Beyond the collection primitives it provides
`nodeDirectory()` (a stable directory for node-level files such as `replication.json`),
may override `holdsNoCollections()` (a follower refuses a data directory that holds
anything else) and `describe()` (one line for the startup log).

A layer derives from `DelegatingDirFactory` and overrides what it changes, usually
`container()` and `incarnation()` to hand out `DelegatingDirectory` subclasses. Both
bases forward every virtual, including `createFile` with options, `expectSynced`,
`deletePrefix`, `sync`, `underlying`, `linkFile`, `openChecksum` and `capabilities`.
A wrapper that changes the semantics of a mutator must override it explicitly.

`LuxirNode::detachCollection` stops serving a collection without removing its storage
(the writer commits what it holds and closes), and `attachCollection` opens one that
storage lists but the node has not opened, as startup does. Together they let a plugin
hand a collection's storage from one node to another. Storage is not touched, so a
layer that writes in the background settles its own directories after the detach, and
it must stop listing a detached collection, or the next startup opens it again.

### Capabilities

`Directory::capabilities()` reports what a backend can do: `HARD_LINK` (`linkFile`
shares bytes), `ATOMIC_RENAME`, `DURABLE_SYNC` (`sync` is an fsync), `MEMORY_MAPPED`
(`openFile` returns stable mapped bytes) and `REMOTE`. The default is none, which
selects conservative paths: `reuseFiles` copies with `Directory::copyFile` instead of
linking when `HARD_LINK` is absent. A wrapper that changes a property (for instance
masks `HARD_LINK`) overrides `capabilities()`.

### Object stores

A plugin that keeps segments in a remote object store implements `ObjectStore`
(`src/luxir/store/ObjectStore.h`) and lets `ObjectStoreDirFactory` do the rest: the
key layout, lazy range reads, staging of writes, the per-segment hotcache and
deletes that wait for readers (see [storage tiers](../design/storage-tiers.md)).

```cpp
class S3Store : public luxir::ObjectStore {
public:
  std::vector<ObjectInfo> list(std::string_view prefix) override;          // sorted; full keys
  std::optional<uint64_t> size(std::string_view name) override;            // HEAD; nullopt when absent
  void readRange(std::string_view name, uint64_t offset, std::span<char> out) override;  // GET with Range
  void put(std::string_view name, std::string_view bytes, bool ifAbsent) override;      // If-None-Match: *
  bool remove(std::string_view name) override;
  void copy(std::string_view from, std::string_view to) override;           // CopyObject
  std::vector<std::string> listDirectories(std::string_view prefix) override;  // optional: a delimiter listing
};

void registerComponents(luxir::Registries& r) override {
  r.storage.addBackend("s3", [](const luxir::StorageContext& ctx) {
    auto store = std::make_shared<S3Store>(ctx.option("bucket").value_or(""));
    return std::make_unique<luxir::ObjectStoreDirFactory>(std::move(store), ctx);
  });
}
```

The contract the directory relies on:

- Every method is thread safe and throws `std::runtime_error` on failure.
- `readRange` throws when the object is missing or shorter than the range. It is
  called from query threads, in chunks of `chunk_bytes`, so it should be cheap to
  issue concurrently.
- `put` replaces the object atomically: a reader sees the old or the new bytes.
  With `ifAbsent` it is a conditional create, and a conflict throws a
  `std::runtime_error` whose message contains `already exists` (collection creation
  is exclusive through it). `put` receives a whole object; split large ones
  (multipart) inside the plugin.
- `list` returns full keys relative to the store, sorted. A plugin with a key prefix
  option strips it again.
- `ObjectStoreDirFactory` reads `chunk_bytes`, `staging_dir` and `spill_bytes` from
  `ctx.options`; the plugin's own options (bucket, region, ...) are read by its
  maker from the same map.

`ram_object` is the built-in backend over `RamObjectStore`, whose request counters
and request log (`requests("read", prefix)`, `bytesRead(prefix)`, `requestLog()`)
let a test assert what a read cost; `RamObjectStore::named(name)` is the store a
`ram_object` node with `--store.option store_name=<name>` uses.
`ObjectStoreDirectoryTest.aReopenedCollectionOpensItsFilesWithoutSizeRequests`
is a small end-to-end example: a node on `ram_object`, reopened, with its
requests asserted through the counters.

An `InputFile` that reads lazily also overrides `rangeStream` and `prefetchRanges`
and returns `true` from `lazy()`; see `LazyInputFile`. Code that opens a segment
must read through `rangeStream`, and code that wants the whole file through `read()`.

### The example plugin

`test/plugins/example` registers a `counting_ram` backend (RAM storage counting
directory opens and creates per collection) and a `counting` layer (counting file
opens and creates). `StorageExampleTest` runs a node on both and asserts the counters
(`CountingStorage::count`).
