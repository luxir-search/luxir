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

For ARM64 builds, `tools/build-arm64` installs extra vcpkg manifest features named in
`LUXIR_VCPKG_FEATURES` (comma-separated; for example `icu` for plugins that link ICU).
`deps/make_deps.sh` reads the same variable for the native install, so plugins link ICU
statically there too.

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
  domain the op hands their calculators (`calcAll` with one `DomainHandle` per
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
