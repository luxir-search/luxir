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
- Each area owns its function-local static registry.
  `luxir::Registries::global()` binds references to them for registration.
  Each constructor seeds its built-ins. `luxir::initPlugins()` adds plugins and
  freezes the registries: later registration throws, and lookups read immutable
  tables without locking. Reads before the freeze are safe while nothing registers.
- One `Plugin` instance per process supplies metadata and registers types once.
  Mutable node-level work belongs in a separate `PluginNodeState` per node;
  registered makers get the current node from their context.
- Registering a name that already exists throws `std::invalid_argument` during
  startup, so a misconfigured binary fails loudly instead of overriding silently.

## Startup order

`initPlugins(addPlugins)` runs after configuration is applied and before any `LuxirNode` or
`Schema` exists (`luxir_main` and the test `main` both call it).
Passing `nullptr` means no plugins. Repeating the same adder, including `nullptr`,
is a no-op after success; calling with a different adder is an error. A failed
initialization is terminal: every later call rethrows the original exception,
even with a different adder. The registries may be partially populated or frozen;
nothing about their state may be relied on afterwards, and the process must exit.

1. Each area constructs its registry on first access and seeds its built-ins,
   so built-in names are taken before any plugin can claim them. Construction
   does not initialize or freeze plugins.
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

A listed directory that does not call `luxir_add_plugin` (for example a plugin that
skips itself when a dependency is missing) fails the configure. Every compiled-in
runtime plugin also embeds the string `luxir-plugin-compiled:<name>`, so
`grep -a` on a binary shows which plugins it contains.

A plugin's third-party dependencies come from optional features of `deps/vcpkg.json`
(for example `icu` for plugins that link ICU). Name them in `LUXIR_VCPKG_FEATURES`
(comma-separated) when running `deps/make_deps.sh`, which installs them for the normal
and ASan builds, or `tools/build-arm64`. Set the variable on every run: an install
without it removes those packages again, and the native installs are shared by all
checkouts. The container image does not install extra features.

`tools/build-arm64` configures from scratch each time, so it takes the plugin
directories from the `LUXIR_PLUGIN_DIRS` environment variable.

Plugin targets see the include directories `luxir_lib` keeps private (generated
protos, hash maps) and link TBB, so a plugin can build on engine internals such as
search ops, readers and `QueryPrep`.

## Node lifecycle

`Plugin::createNodeState(LuxirNode&)` runs at the end of each node's construction,
after its collections are open, in registration order. The default returns null
for a plugin with no node state. Otherwise return a `std::unique_ptr` to a
`PluginNodeState` subclass (defined in `luxir/server/PluginNodeState.h`): its
construction starts the work, and its `noexcept` destructor stops it. The node
owns these states, independently of every other node in the process. Factories
may run concurrently for different nodes; keep their mutable work in the states.

If creation throws, the node destroys completed states in reverse order and
rethrows. The throwing factory must use RAII to clean up its own partial work;
factories after it are not called. Node shutdown explicitly destroys the states
in reverse order first, before stopping the scrubber, follower and commit waits.

`node.pluginState<T>("my_plugin")` looks up the state by plugin name, using a
checked cast. It returns null if the plugin is absent, returned no state, or the
state has a different type. Registered makers must look up their node's state
per call rather than capturing one node's state in a process-wide registration.

Both SERVE and PULL nodes create states. `node.getMode()` returns
`LuxirNode::Mode::SERVE` or `PULL`, so a state can decide which resources to start.
The example plugin records its node's mode in its state.

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
  boolean, ...) or a `PluginQueryBase` subclass (include
  `luxir/query/PluginQueryBase.h`), constructed with the maker's `ctx`.
  The base sets `QueryKind::PLUGIN`, which every planner shape question answers
  conservatively. It borrows the registry's stable name and final-overrides
  equality, hashing and filter keys to include both that name and the concrete
  C++ type. Filter keys use an interned, collision-free process-local type token;
  they are only used by the in-memory cache. Implement `equalsPayload`,
  `hashPayload` and `appendPayloadFilterKey`; the equality payload hook sees
  only the same concrete type and registered name. Keep payload members
  trivially destructible too. A PLUGIN-kind result without this base is rejected
  at the maker boundary for both structured and expression queries. A subclass
  that depends on documents across segments returns a reader-wide filter key
  scope (`READER_STABLE`) from `appendPayloadFilterKey`.
- `ctx.parse.fields.note(text)` adds a line to `?explain=resolved`, for
  example what the query resolved its parameters to.
- `ctx.parse` is the request's `ParseContext`: pool, schema, the searched reader
  (`ctx.parse.reader`, null outside a search), coercion context, warnings and field
  resolver, which is everything `QueryBuilder` needs.
- `ctx.node()` returns the request's node, including for expression and nested
  queries. It returns null for offline parsing without a node (`ParseContext::node`
  defaults to null); makers using node state must handle that case.
- `ctx.parseQuery(...)` lowers a nested `api::Query`, or a Query-shaped param (an
  object or an expression string), through the ordinary parser with the shared
  nesting budget.
- `ctx.params` (`ParamReader`) reads typed params and `rejectUnknown()` refuses the
  rest; every error names the type and the parameter. The neutral reader throws
  `ParamError`, derived from `std::invalid_argument`. Query and op maker boundaries
  translate `ParamError` to `RequestError` with the same message.
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

- `view` is the operation's scoped reader pin. Pass it to `SearchOp(req, view, name)`
  and to document emission; the request's primary reader can be in another space.
- `node()` returns the request's node by reference. Its `pluginState<T>(name)`
  accesses the same state that query makers and HTTP handlers see.
- `parseQuery(...)` lowers and validates a query against the op's `planning()`
  context; `weightContext()` is the `Query::Context` for `createWeight`, and
  `planning().getFilterUse(query)` its filter cache entry.
- `parseSource(name, val)` parses a TopDocs-shaped param as a source the op runs
  itself, as `FusionOp` runs its sources: set its `rankingSink` and `parent`,
  create its calculator under the op's own, dispatch it per segment and receive
  the merged collector.
- `addSubs(op)` parses the nested ops into `op.subOps`. They run over whatever
  domain the op hands their calculators (`acceptAll` with one `DomainHandle` per
  segment; an empty set, never null, for a segment with no documents), and may not
  declare their own `domain`.

The op's calculator follows the `SearchOp::Calculator` contract (see
`src/luxir/search/ops/SearchOp.h` and `FusionOp.h`). Results are a `Val` in the
response: `emitDocsResponse` (`EmitDocs.h`) writes a `DocList` with returned
fields, and its `extraColumns` add computed per-document values beside them
(`ExtraColumn{name, value(i, out, mr)}`). Extra columns are always returned; a
`fields` selection may name them. An explicit selector naming an extra selects
that extra even if a physical field has the same name, and takes the normal
explicit-selector precedence over matching wildcards. Other selectors are
resolved normally. A selection made only of extras does not project default
fields; an empty selection still does. If default or wildcard discovery finds
a field whose output name matches an extra, the request fails: use an explicit
field list to omit that physical field or select the extra by name. `_score_`
and `_version_` are reserved output names and cannot be extras. Duplicate extra
names are a plugin error (`std::logic_error`); field clashes are `RequestError`s.
All checks run during resolution, before any column or loader is created.
A free-form result is a `Map`. Nested op
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
  `request.node`, which also supplies `pluginState<T>(name)`. It answers with
  `status`, `contentType` and `body`, or sets
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
`luxir_test` only). It registers `doc_ids` (lowering to built-ins), `field_value`
(two concrete PLUGIN query classes), an alias to test registered-name identity,
and `unscoped_value` to test rejection of a PLUGIN query without the base.
It also registers a `sample_docs` search op
(rank columns named by `rank_names`, defaulting to `_rank`, and nested ops over
its documents), two HTTP routes and a per-node state with query, op and route
counters. The makers and handlers reach that state through their contexts.
`PluginExtensionTest` and `PluginQueryTest` exercise the plugin end to end,
including independent live nodes; `PluginTest` covers registry validation and
state creation/destruction order.

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
  JSON that field signatures and the resolved view use. The stored schema keeps the name and expands it
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

The example plugin registers an `example_reverse` filter (reverses token bytes, `normalizing`), a
`strip_x` char filter (removes every `x` through `replace`) and the analyzer `example` =
`whitespace` + char filter `strip_x` + filters `lowercase`, `example_reverse`:

```cpp
void registerComponents(luxir::Registries& r) override {
  r.analysis.addFilter("example_reverse", [](const luxir::api::AnalyzerComponent& def) {
    struct Factory : luxir::TokenFilterFactory {
      std::unique_ptr<luxir::TokenStream> create(std::unique_ptr<luxir::TokenStream> source) const override {
        return std::make_unique<ReverseFilter>(std::move(source));
      }
    };
    auto factory = std::make_unique<Factory>();
    factory->name = "example_reverse";
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
(`"analyzer": {"tokenizer": "whitespace", "filters": ["example_reverse"]}`).
Text is fed to a chain with `TokenChain::setValue`, never `head.setValue`, so char filters
always run.

### Token attributes

Besides `text`, `positionIncrement` and the offsets, a `Token` carries `positionLength` and
the class and script of its input word:

| Attribute | Default | Meaning |
|---|---|---|
| `positionLength` | 1 | Positions the token spans: its arc length in the token graph (Lucene's `PositionLengthAttribute`), 2 for a synonym covering two source words. `uint16_t`, reset by `Token::clear()` when a tokenizer starts a token. |
| `wordClass()` | | `WordClass`: `ALPHA` (letters, ideographs included), `NUMERIC` (digits), `ALNUM` (both), `OTHER` (neither, such as emoji). |
| `script()` | | `Script` of the first letter that has one: `LATIN`, `HAN`, `HIRAGANA`, `KATAKANA`, `HANGUL`, `THAI`, `ARABIC`, `CYRILLIC`, `GREEK`, `OTHER`; `COMMON` when there is no letter (digits) or no letter belongs to a script. |

Letters are the code points of general category L*, numbers those of N* (so U+2163 ROMAN
NUMERAL FOUR is numeric); marks, symbols and punctuation inside a word are neither.
`scriptOf(char32_t)` is the per-code-point script lookup.

`wordClass()` and `script()` describe the token's input word: the tokenizer input bytes in
`[startOffset, endOffset)`, which `Token::inputText()` returns (after char filters, before any
token filter). A filter that rewrites `text` does not change them: U+2163 folded to `iv` by
`nfkc_cf` is still `NUMERIC`. A filter that wants the class of some other bytes, such as the
text it produced, calls `classifyWord(std::string_view)`, which returns both. A CJK bigram
filter can branch on `token.script() == Script::HAN` instead of guessing from the first byte.

Both are computed on the first read and cached in `knownClass` and `knownScript` (`UNKNOWN`
until then), so a chain whose filters never ask pays nothing. `unicode_word` stores them for the
words its ASCII scan already classified; every other word, and every token of `whitespace`,
`keyword` or a plugin tokenizer, is classified from its input bytes when a filter first asks.
A plugin tokenizer that knows a token's class can store both fields itself, after
`setOffset`.

`Token::setOffset` resets the cache: a new span is a new input word. A filter that splits or
merges tokens gives each its span with `setOffset` (never by assigning the offsets), and
readers then see the class of the new span. A buffering filter that restores a saved token restores its
offsets with `setOffset` and gets the class recomputed on read, unless it also saved and
restores `knownClass` and `knownScript` after the call.

`positionLength`, `knownClass` and `knownScript` sit next to `positionIncrement`, so `clear()`
resets all four with one 8-byte store (`Analyzer.h` has a `static_assert`).

`positionLength` is carried and nothing reads it yet. Postings store only each term's start
position (as Lucene's do), and phrase queries are built from position increments, so a
token spanning several positions indexes and matches exactly like a one-position token.
Honouring it needs a query-side graph (a phrase over alternative paths); a filter that
produces graph tokens can set it today and a later query builder can consume it.

`TokenAttributesExampleTest` shows a plugin filter (`describe`) reading all three, through
`wordClass()` and `script()`, on fused and unfused chains, and the example `span2` filter
setting `positionLength`.

## Storage backends and layers

`Registries::storage` (`src/luxir/store/StorageRegistry.h`) holds named storage
backends and layers. `ram`, `fs` and `shared_fs` are built in and registered before plugins.

- A **backend** builds the root `DirectoryFactory` from the node configuration. It
  is selected by `--store.backend NAME`.
- A **layer** wraps the factory built so far. Layers are selected by
  `--store.layer NAME` (repeatable, innermost first) and sit between the backend and
  the core wrappers: `checked_dir` and `read_only` stay outermost, so a mutation is
  refused before any layer sees it.

Both makers receive a `StorageContext`: the whole `LuxirConfig`, `params` (a
`ParamReader` over only that component's options), and `reclaim`, the callback RAM
backends call under memory pressure. The context's references are valid only for
the maker call; copy any values the factory needs to retain.

Pass options as `--store.option COMPONENT.KEY=VALUE`, where `COMPONENT` is the
backend or layer name (`[a-z0-9_]+`) and `KEY` is non-empty. Missing components,
keys, dots or equals signs fail parsing. Values that parse as JSON integers,
doubles, booleans or quoted strings retain that type; everything else is a bare
string. For example, `s3.bucket=logs` and `cache.bytes=10G` are strings,
`cache.bytes=1048576` is an integer, and `--store.option 's3.bucket="123"'`
preserves a numeric string (the shell quotes keep the JSON quotes intact).
`getBytes()` accepts non-negative integers or strings using the same size syntax
as CLI size options, with binary units (`KB` is 1024 bytes).

After the makers return, `Collections` calls `rejectUnknown()` on their readers.
An unknown key names its component and lists the keys the maker read; `ram` and
`fs` take no parameters. Options for an unconfigured component fail startup and
list the configured components. Repeated layers receive the same component's
options, and each maker call validates them independently. Repeated assignments
to a component key use the last value. Unknown backend or layer names list the
registered names.

```cpp
void registerComponents(luxir::Registries& r) override {
  r.storage.addBackend("s3", [](const luxir::StorageContext& ctx) {
    return std::make_unique<S3DirFactory>(std::string(ctx.params.getString("bucket")), ctx.config.read_only);
  });
  r.storage.addLayer("cache", [](std::unique_ptr<luxir::DirectoryFactory> inner, const luxir::StorageContext& ctx) {
    return std::make_unique<CacheLayer>(std::move(inner), ctx.params.getBytes("bytes", 1ULL << 30));
  });
}
```

A backend implements `DirectoryFactory` over two roots: `AUTHORITATIVE` for the
portable `LAYOUT` and `t/` tree, and `NODE_PRIVATE` for local files such as
`write.lock` and `replication.json`. `directory(root, path, options)` opens a
relative path view without creating namespace entries; `children(root, path)`
lists children, and `removeTree(root, path)` removes a subtree durably. Paths
cannot escape their root. Collection names, catalogs and incarnations are engine
logic over these views, not backend primitives.

`memory()` exposes the backend's `StorageMemory` account when it has one. A view
receives its opaque accounting scope in `ViewOptions::account`, and reopening
must not charge the same bytes twice. `describe()` supplies one line for the
startup log; `diskPath()` supplies a local path for disk statistics, or empty.
FS reserves `node/` from authoritative views. The private root is `node/` for
`fs` and `node/<id>/` for `shared_fs`.
A backend must either honor `ctx.config.read_only` by opening without a lock,
namespace creation or cleanup, or reject it. The read-only wrapper cannot undo
side effects inside the maker; the built-in RAM maker rejects read-only.

`exclusive()` defaults to false. Return true only while this process owns the
whole authoritative store (private RAM, or FS with its store write lock). An
exclusive startup adopts every catalog entry and cleans up abandoned trees.
`DelegatingDirFactory` does not forward this method: a layer must explicitly
forward it only if it preserves that guarantee. Incorrectly returning true can
delete another node's data; returning false only costs availability. The core
wrappers and the example `counting` layer forward it; `counting_ram` is exclusive.

### Reading files

`Directory::openFile` returns an `InputFile` with `size()`, advisory
`prefetch(offset, length)`, and `acquire({offset, length})`. Reject ranges
outside the file and return a move-only `HeldRange` for valid ranges. Its data
address must be congruent to the file offset modulo 16. Make the next 16 bytes
after the range readable up to EOF, with unspecified contents.

Keep each range readable until its hold is released. If all holds are
null-owner views valid for the lifetime of the file, as in RAM and mmap,
return true from `holdsFree()`. Otherwise give the hold a `HoldOwner` and any
token needed to release it. Destruction or replacement of the hold calls
`HoldOwner::release(const HeldRange&)`, which must be thread-safe and
`noexcept`. The caller releases the hold before destroying its `InputFile`.

`openFile(name, OpenOptions)` returns null when the file does not exist,
whether or not a size was given. A nonzero `OpenOptions::size` is the size
the manifest records; after such an open, `size()` must not make a request.
Answer it from the given size or from a cached listing. Wrappers forward the
options.

Keep an open `InputFile` readable until it is destroyed, even after its file
is deleted. A snapshot opens its live docs and overlay files and reads them on
first use, which may come after the writer has retired them for a newer
commit. Local files get this from the open descriptor or mapping; a remote
backend can pin the object, for example with a count of open readers.

A remote directory can keep a cached listing of its prefix (names, sizes,
ETags) and answer `openFile` and `size()` from it, listing again when a name
is missing, so neither existence nor size costs a request per file. With a
passed size it needs the listing only for existence.

Each `ByteRange` also carries a `ReadClass`, `METADATA` or `DATA`. Whoever
keeps a hold decides how long it lives: the segment for what it keeps, a
request for what it uses. The class is a hint a cache or backend may use for
residency and scheduling after release, and it never changes the bytes
returned.

`acquireBatch(ranges, done)` takes a batch of ranges on one file. The
default acquires each range in turn and calls `done` inline, so a local
backend needs nothing more. A remote backend can override it to merge the
ranges and issue them as one round of requests. It reports every outcome
through `done`, which it calls exactly once, possibly before returning, from
any thread: with one hold per range in the order requested and a null error,
or with no holds and the error. `acquireBatch` is `noexcept`, so a failure to
issue the batch (a bad range, an allocation) also goes to `done`, and `done`
does not throw. The ranges are valid only during the call, so copy what the
completion needs. Wrappers forward the batch to their delegate rather than
splitting it, so the backend sees it whole.

A search over files that are not `holdsFree()` declares the parts it will
read in each segment and acquires them in rounds of batches: the term routing
data (block offsets and trie) and live docs, then the term blocks its lookups
land in, then the postings of the terms found, then what its scorers read
(norms). Each round is one batch per segment file, issued together across
segments, so a backend that completes batches asynchronously serves a cold
query in a few round trips however many segments it has. No search worker
waits for a round: the search resumes on the node's arena when the last batch
completes, so `done` may run on a thread of the backend's own. A part of at
most 64 KiB (`field_parts::WHOLE_READ_BYTES`), such as a small dictionary, is
acquired whole. Past `search.read-ahead-max-bytes` per request, parts are
acquired on demand instead. A round still pending at the request's
`waitDeadline()`, or when it is cancelled, fails the search; its batches may
still complete afterwards, and their holds are kept.

### Warming a snapshot

`IndexReader::listParts(fields, allData)` lists the parts of every segment of
a snapshot without reading anything: each file's header and the catalog
(`CATALOG`), the navigation parts of every field in every row space
(`NAVIGATION`: term routing data, column indexes, ordinal block metadata,
points directories, stored field names, id filters, version runs and
ownership columns), and the data extents of the named fields, or of all
fields with `allData` (`DATA`: dictionaries, postings, norms, columns, points
leaves, stored chunks). Each entry gives the file name, offset and length, the
`ReadClass` the readers acquire it with, the row space and the field. These are
exactly the ranges reads acquire, so a backend can warm metadata from the
first two kinds, or a listed field's data from the third, and serve the reads
that follow from its cache:

```cpp
// Metadata of every field, and the data of two fields.
std::string_view fields[] = {"title", "body"};
for (const auto& part : collection.getReaderManager().getReader()->listParts(fields)) {
  warm(part.file, part.offset, part.length, part.readClass);
}
```

### Storage layers

A layer derives from `DelegatingDirFactory` and overrides what it changes, usually
`directory()` to hand out `DelegatingDirectory` subclasses. Both
bases forward their other virtuals, including `createFile` with options, `expectSynced`,
`publishFile`, `readPublished`, `deleteDurably`, `deletePrefix`, `sync`,
`underlying`, `linkFile`, `openChecksum` and `capabilities`.
Core wrappers that refuse or track mutations implement the bare interface;
layers derive from the delegating bases. `ReadOnlyDirectory`, `ReadOnlyDirFactory`
and `CheckedDirectory` therefore implement their interfaces explicitly. New pure
virtuals require an implementation there; default methods call their own virtuals.
`CheckedDirFactory` still delegates because it wraps views rather than policing
namespace operations.

Layers must be **byte-transparent**: the bytes at rest must be those the engine
wrote, in a complete, durable namespace readable by the backend. RAM and FS
resolve the `linkFile` source through `underlying()`. Offline promotion opens
`FSDirFactory` directly, and replication compares file digests. Observation,
counting and caching fit this contract; encryption or compression at rest must
be a backend, not a layer.

Storage makers run before `PluginNodeState` is created. Keep storage state on the
factory instance, shared with its views as needed. Once the node exists, its plugin
state or routes can use `node.collections().storage().find<MyFactory>()` to reach
that state through the core wrappers. `find<T>()` checks each factory with
`dynamic_cast` and follows `inner()` to its delegate, returning null if absent.
Repeated layers return the outermost matching type. The pointer is borrowed for
the factory's lifetime; process-wide plugin objects must not retain it.

Placement plugins call the engine hooks `LuxirNode::detachCollection(id, target)`
and `LuxirNode::attachCollection(id, force)` to hand off writers or force a takeover.

### Capabilities

`Directory::capabilities()` reports what a backend can do: `HARD_LINK` (`linkFile`
shares bytes), `ATOMIC_RENAME` (the backing store supports atomic name
replacement), `DURABLE_SYNC` (`sync` persists bytes and names), `MEMORY_MAPPED`
(the backing storage provides mapped bytes) and `REMOTE`. The default is none, which
selects conservative paths: `reuseFiles` copies with `Directory::copyFile` instead of
linking when `HARD_LINK` is absent. A wrapper that changes a property (for instance
masks `HARD_LINK`) overrides `capabilities()`.

### The request I/O context

A read done for a search can see that search through `luxir::IoContext`
(`src/luxir/util/IoContext.h`): `IoContext::current()` returns it, or null
when the read is not done for a request (merges, startup, reader refresh,
verification, replication). It is visible in `InputFile::acquire` and in every
`DelegatingDirectory` method called on a query thread, including
segment-parallel tasks, since core carries it into the tasks it spawns.

```cpp
luxir::HeldRange MyInputFile::acquire(luxir::ByteRange range) {
  const luxir::IoContext* io = luxir::IoContext::current();
  bool hedge = io && io->remaining() < hedgeBudget;  // deadline: hedge a slow GET early
  bool admit = !io || io->fillCache;                 // a scan: do not admit into a cache
  ...
  if (io) io->countFetch(bytes);                     // one GET, one round
}
```

The fields are the query id and `request_id` (for correlating a layer's logs),
the deadline (`hasDeadline()`, `remaining()`, `waitDeadline()`), the stop
token, the `fillCache` hint and the request's `IoCounters`. The deadline and
the hint are advice: a read must return the same bytes, and core never fails a
read on them. A layer that gives up a wait on the deadline or the stop token
throws `DeadlineExceededError` or `RequestCancelledError`.

Core counts every range a segment acquires from a file that is not
`holdsFree()` (`io_acquires`, `io_acquired_bytes`), so those counts appear on
any such backend without its help. What a range costs in store requests only
the backend knows: it counts them with `io->countFetch(bytes, requests,
rounds)` (`countFetch(bytes)` is one request and one round), counting a wave
of concurrent requests as one round. Counts appear in the search profile
(`io_*`) and in the node's `search_io_*` totals.

Work handed to a thread or pool of the plugin's own runs without a context
unless it is wrapped: `pool.submit(luxir::bindIoContext(task))` copies the
current context into the task and restores the pool thread's own afterwards.
`IoScope` is the underlying guard; `IoScope scope(nullptr)` detaches work from
the enclosing request. The counters belong to the request and outlive its
tasks, but not the request: work still running after the request ends must
not count.

### The example plugin

`test/plugins/example` registers a `counting_ram` backend (RAM storage counting
directory view opens by path) and a `counting` layer (counting file
opens and creates). Each factory owns its counters; directory views share the
layer's counters. `StorageExampleTest` reaches `CountingRamBackend` and
`CountingLayer` through `find<T>()` and verifies that two nodes count independently.

Run `luxir_test --store.layer=strict` to check readers against isolated range
buffers with varying garbage in the slack. It reports acquisition counts
(total and metadata), size requests on files opened without a size, and peak
live holds and bytes. A part acquired from one open file under both read
classes is an error in either mode. The default
`strict.whole_segment_files=count` allows whole-file reads for tests that copy
or replicate files. Use
`--store.option strict.whole_segment_files=deny` in focused reader tests to
reject whole-file reads of base segment data files. In those files a read no
part makes also counts as whole-file: one from offset 0 past the header, or
one to EOF outside file 0, since the writer pads every other file after its
last part.

## Result decorators

`Registries::hitDecorators` registers a maker receiving a `DecoratorContext`
(request, query parse context, and original ranking queries with execution
contexts) and a `ParamReader`. Return a shared `HitDecorator` with a unique output column name.
Register during startup; the registry is frozen with the other registries.

TopDocs and Fusion accept `decorate: {"registered_name": {params...}}` on the
wire. JSON also accepts a registered name directly as a list key. Unknown names,
non-object params, duplicate decorators and unknown params are request errors.
Candidate Fusion sources cannot decorate; configure decoration on the Fusion.

`HitDecorator::decorate(docs, rows, out, mr)` runs only over emitted rows, after
paging. Each call receives a sorted segment run within a response batch, with
`rows` mapping into the emission-order `docs` and `out` arrays. Calls for different
runs may execute concurrently. Leave `out[row]` null when there is no value.
Allocate response-owned strings and arrays using `mr`; do not retain stored-reader
views past their callbacks. Columns flow through the ordinary ROWS, COLUMNS and
ADAPTIVE placement and streaming paths.

Keep mutable readers and forward-only iterators call-local: sorting is per batch,
and a later ranked batch can contain lower document IDs. Immutable per-request
plans may be shared with synchronization during initialization. Decorators cannot
change membership, order or scores. The output name must not collide with another
extra column. Decorators are also invoked for nested top-doc lists, collapsed
heads, and diversified lists.

### Inspecting the ranking queries

In the maker, call `source.query->visit(visitor, source.context)` for each entry
in `DecoratorContext::queries`; a null query means browse. `QueryVisitor`
(`luxir/query/QueryVisitor.h`) reports indexed bytes through `term(field, text,
boost)`. `sub(occur, parent)` chooses the visitor for a composite's children or
returns null to skip them; the default skips MUST_NOT. Inspectors interested only
in ranking terms should also skip FILTER. `leaf(query)` means the term picture
is incomplete. Plugin queries can override the protected `Query::visitImpl`
to participate without exposing their concrete tree.

Inspection runs only during serial planning, in the maker. It may expand fuzzy
terms, cache the expansion shared with execution, and throw the request's
multi-term budget errors. Never call visit from concurrent `decorate()` calls.
Term views borrow query/context storage; copy what decorate needs into the
decorator. The example `ranking_terms` decorator does this (it lists request
ranking terms, not the terms matched by each hit).

Traversal preserves raw Boolean occurrences and authored dis_max disjuncts.
Boosts mirror logical validation and execution: filter children reset the
multiplier; rescore and min_score inspect their matching child at multiplier 1.
Rescore value expressions are not term queries and are not inspected. Combined
field weights multiply term frequency before BM25 saturation; the reported
path/field/term product is not a linear final-score contribution. Phrase terms
omit positions. Explicit TermInSet terms are enumerable in query-size time.
ALL and NONE report no terms and no leaf. Other opaque queries report leaf.
Case-insensitive MATCH shows execution's representation: leaf until a scored
weight has populated the shared case-variant rewrite, then its variant terms.
Inspection never forces that rewrite or changes later unscored execution.
