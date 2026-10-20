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

## The example plugin

`test/plugins/example` is a test-only plugin (always configured, linked into
`luxir_test` only). It exercises the framework in `PluginTest`, and later
extension points add their end-to-end tests to it.

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
  JSON that segment metadata persists.
- The fused `unicode_word` + `nfkc_cf` fast path applies only to exactly those built-in
  components; plugin stages always run as written.
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
