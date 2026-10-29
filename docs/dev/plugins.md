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

## Sort kinds

`Registries::sortKinds` (`src/luxir/search/SortKindRegistry.h`) lets a plugin
sort with its own comparator. A plugin claims a function name; a sort clause
whose whole expression is a call to that name, `name(args...)`, or that call
under `min(...)` or `max(...)`, is the plugin's:

```json
{"sort": ["min(abs_diff(sizes_is, 42))", "id"]}
{"sort": {"expr": "abs_diff(size_i, $target)", "vars": {"target": 42}, "dir": "desc"}}
```

- The claim decides only that root form. Anywhere else, including a larger
  expression such as `abs_diff(size_i, 42) + 1`, the name is read by the value
  language, so a plugin can claim the name of a value function and sort its
  root form faster while the function keeps working inside expressions. Unclaimed
  expressions sort as before.
- Arguments are literals: a bare name (usually a field), a number, a quoted
  string, or a `$variable` from the clause's `vars` holding a number or a
  string. Anything else (a nested expression) is a request error naming the
  argument.
- `min` and `max` reach the maker as `ctx.mode` (`SortKindMode::MIN`, `MAX`;
  `SINGLE` without one). What they mean is the kind's; for a multi-valued
  field they usually pick the nearest or farthest value, and a kind can
  require one there.
- Names are `[a-z0-9_]+`, do not start with a digit, and may not be `min`,
  `max` or `col`. A duplicate, a null maker, or an add after the freeze
  throws.

The maker receives a `SortKindContext`: the claimed `name`, the whole `expr`,
`mode`, `descending` and the parsed `args`, with accessors that check and
convert them (`arity`, `field(i)`, which resolves a field as value expressions
do, `number`, `integer`, `string`), `fail(message)` for a request error naming
the expression, `note(text)` for `?explain=resolved`, and the request's
`schema` (read-only) and field resolver. Whitespace may separate a name from
its `(`: `min (abs_diff (x, 1))` is claimed too. It returns a `SortKindSource`, shared by every
collector the request makes (threads, collapse member folds, `head_sort`), so
it is immutable:

```cpp
class AbsDiffSource : public luxir::SortKindSource {
public:
  std::unique_ptr<luxir::FieldComparator> createComparator(luxir::IndexReader& reader) const override;
  void value(luxir::IndexReader& reader, const luxir::IndexReader::Segment& segment, int32_t doc,
             luxir::api::Val& out, std::pmr::memory_resource& mr) const override;
};

r.sortKinds.add("abs_diff", [](luxir::SortKindContext& ctx) -> std::shared_ptr<const luxir::SortKindSource> {
  ctx.arity(2, 2);
  const auto& field = ctx.field(0);
  // ... check the field type, refuse SINGLE on a multi-valued field ...
  return std::make_shared<AbsDiffSource>(field.physicalName, ctx.integer(1), ctx.mode, ctx.descending);
});
```

- `createComparator` makes a `FieldComparator` (`src/luxir/search/FieldComparator.h`)
  over the reader, in the clause's direction. The collector calls
  `setSegment`, then `copy`, `compare`, `compareBottom` and `setBottom` with
  slots it grows through `growSlots`, and merges per-thread collectors through
  the two-comparator `compare` and `copy`. Docs without a value rank last in
  both directions, tied among themselves; ties fall to later clauses, then to
  document order. `ValueSortComparator<T>` implements all of it for one
  `int64_t` or `double` key per doc: the subclass sets up its readers in
  `setSegment` and reads a doc's key in `read(doc, out)`, called in any doc
  order.
- `value` gives a doc's sort value (the key the comparator ranks by) for the
  `_sort_` return field and collapse ranks (`_group_.rank`). Leave `out` unset
  for a doc without one. It must agree with the comparator: the same key, and
  no value exactly for the docs the comparator ranks as missing.
- A plugin clause works as any other: as a primary or secondary clause, under
  collapse, `head_sort` and diversify, and with `offset` paging. The routes
  that read a sort column's leaf bounds and value envelope apply to column
  clauses only; a plugin prunes through `competitiveDocs`.

A comparator that knows where its competitive docs are can let the collector
jump over the rest: it returns true from `offersCompetitiveDocs()` and answers
`competitiveDocs(bottomSlot, from, sourceDocs, begin, end)`. When the plugin
clause is the primary one, the heap is full and the query permits pruning, the
collector asks it for the first run `[begin, end)` at or after `from` that may
still enter the heap (`begin == END`: none in this segment), and advances the
query to `begin`. Set both ends, with `from <= begin < end`; the collector
clamps a run that starts before `from` or holds no doc, so a malformed answer
cannot stall the query or repeat docs. The bottom comes as a slot, without its
doc, so docs tied with it must stay in the run. `sourceDocs`
estimates the docs the query would still produce, the work a lookup has to
undercut. `PENDING` means not yet (no bound, or one too wide to pay off; an
`end` past `from` names the doc to ask again at), and `UNSUPPORTED` means
never in this segment.

`competitiveDocs` is consulted only once the heap is full, and the bottom is
only as good as the docs the scan has met. When doc order is unrelated to the
key (or opposed to it, as in a spatially sorted segment whose nearest docs come
last), the bottom matures late and the prune fires late. The comparator cannot
seed a bound itself: it does not see the query, and the k-th best key over all
docs is no bound over the matches. Instead it can offer seed windows: it
returns true from `offersSeedWindows()` (asked after `setSegment`, so per
segment) and answers `seedWindows(request, out)` with a few doc runs
`[begin, end)` likely to hold the segment's best keys (a geo distance sort: the
docs of the BKD leaves nearest the origin). Before a plain sweep of the segment
the collector runs the query over those runs, so the heap fills with real
matches only, then rewinds the same query to the segment start and sweeps it
through `competitiveDocs`, jumping the runs it already collected. Only the visit
order changes, so the results are the unseeded ones for any query, ties and
document order included.

- `request.matches` is the heap's depth and `request.sourceDocs` estimates the
  docs the query produces in the segment: about
  `matches * maxDoc / sourceDocs` docs hold the matches the heap needs.
  `request.bottomSlot` is the heap bottom when earlier segments filled it,
  else -1. Leave `out` empty to sweep unseeded.
- Seeding runs in rounds. `sourceDocs` is an estimate (a range over a tenth of
  the values may be estimated at half the docs), and a filter can be much
  sparser near the best keys than overall, so a round's runs may leave the
  heap short; then the sweep could not prune until it filled, and on a
  segment ordered by the key it scans most of the segment. So while the heap
  is not full, the collector asks again, up to four more rounds:
  `request.round` counts from 0 and `request.collected` is the docs the heap
  holds. A later round should offer the next ring beyond what the comparator
  offered before, growing geometrically (at least doubling, or scaled by
  `matches / collected`), rather than re-offering or over-seeding up front.
  Runs an earlier round visited are dropped from the answer, and an empty
  answer, or one with nothing new, ends the seeding. Between rounds the
  collector rewinds the query and binds the later clauses again, and the
  sweep jumps the runs of every round.
- Any answer is safe: the collector clamps the runs to the segment, drops
  empty ones, sorts and merges overlapping ones, and drops what earlier
  rounds visited, so a doc is collected once.
- The comparator reads docs out of order (the runs, then the sweep from doc 0);
  `ValueSortComparator::read` must already allow that. The collector binds the
  later clauses' comparators to the segment again before the sweep, and
  collapse and diversify identities reset on a backward read, so a seeded
  primary clause works under both.
- The collector seeds a query-driven sweep it can rewind, with no score or
  value-expression clause and no exact count or domain to produce. That is
  the windowed sweep whose product can reposition (match-all, terms, Booleans,
  ranges, cached filters), and the pull-scorer sweep a segment takes when the
  query has no match-window product, such as an uncached filter over a
  column-only field: the scorer advances through the runs, is reset to the
  segment start (its plan must support reset) and sweeps through
  `competitiveDocs`, jumping the runs. The column routes that seed from a column's
  leaf bounds, and plans that cannot reposition or reset, stay unseeded. There
  is no separate hook for a known exact domain (match-all, filter-only):
  running the windows through the query already holds the heap to real
  matches, and `sourceDocs` sizes the runs for the domain's density.
  `SkipStats::fieldSortSeedWindowActivations` counts seeded segments,
  `fieldSortSeedWindowPullActivations` those seeded on the pull path and
  `fieldSortSeedWindowRounds` the rounds after the first.
- `LUXIR_DISABLE_FIELD_SORT_SEEDING` (`disableFieldSortSeeding`) turns it off
  along with the column seeding, for A/B runs.

The example plugin registers `abs_diff(field, center)`, which sorts an INT
field by `|x - center|` (`min()` and `max()` for a multi-valued field, which
requires one). Ascending on the nearest value, its competitive docs lie within
the bottom's distance of the center: it counts them from the field's points
index (`"index": "range"`), reads them once they are under an eighth of
`sourceDocs` and again each time the bottom distance halves, and counts the
docs it ruled out in `example::AbsDiffStats` (`test/plugins/example/ExampleSort.h`).
It seeds from the points index leaves holding the points nearest the center,
enough for the heap at the query's density; each later round widens the
leaves offered so far by `matches / collected`, at least doubling them, and
offers the new ring. It counts the seeded segments, later rounds and windows
there too.
`PluginSortTest` checks it against brute force, against the value
expression `abs(x - center)`, which sorts the same docs without pruning, and
seeded against unseeded, including a corpus whose nearest docs come last,
on both the windowed and the pull-scorer path, and one whose filter is
sparse near the center, where round 0 leaves the heap short.

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
It also registers field types (see [Field types](#field-types)) and a `sample_docs` search op
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

## Field types

`Registries::fieldTypes` (`src/luxir/schema/FieldTypeRegistry.h`) maps names to makers of
field types. A plugin field type maps one authored field to a set of *derived* built-in
fields, so it needs no on-disk format of its own: indexing, merges, deletes, codecs and
replication of the derived fields are those of ordinary fields.

```cpp
class PairType : public luxir::PluginFieldType {
public:
  PairType() {
    fields = {{.suffix = "a", .kind = luxir::DerivedField::Kind::INT},
              {.suffix = "b", .kind = luxir::DerivedField::Kind::INT},
              {.suffix = "key", .kind = luxir::DerivedField::Kind::STRING},
              {.suffix = "packed", .kind = luxir::DerivedField::Kind::STRING, .index = false,
               .opaque = true}};
  }
  void index(const luxir::api::Val& value, luxir::DerivedSink& sink) const override {
    // value is the raw document value: an object here. Throw
    // std::invalid_argument for one the type cannot read.
    sink.integer(0, a);
    sink.integer(1, b);
    sink.string(2, key);
    sink.string(3, packedBytes);
  }
  bool projects() const override { return true; }
  void project(const luxir::DerivedValues& values, luxir::api::Val& out,
               std::pmr::memory_resource& mr) const override {
    // rebuild {"a": ..., "b": ...} from values.get(3), allocating in mr
  }
};

void registerComponents(luxir::Registries& r) override {
  r.fieldTypes.add("pair", [](const luxir::ParamReader& params) -> std::unique_ptr<const luxir::PluginFieldType> {
    auto separator = params.getString("separator", ":");  // unread params are refused afterwards
    return std::make_unique<PairType>(/* ... */);
  });
}
```

A schema names the type in place of a built-in one, with its settings in `params`:

```json
{"fields": {"pt": {"type": "pair", "params": {"separator": "/"}, "ignore_malformed": true}}}
```

A type name that is not built in reads as `plugin_type` (`FieldDef.plugin_type`, a name
and params), which is the canonical form the schema echoes:
`{"plugin_type": {"name": "pair", "params": {"separator": "/"}}}`. Only `parent` and
`ignore_malformed` combine with it, authored or inherited (a parent with built-in settings
is refused); the type declares everything else. Templates work
(`"templates": {"_pair": "pair"}` makes `x_pair$a` for a document key `x_pair`); variants
and defaults do not.

- **Registration.** Names are `[a-z0-9_]+`, not a built-in type name and not `plugin`; a
  duplicate or an add after the freeze throws. The maker reads its params through
  `ParamReader` (errors name the type and the parameter), and any param it did not read is
  refused after it returns. Makers throw `std::invalid_argument` or `RequestError` with a
  message that teaches; the schema adds the field name.
- **Derived fields.** `PluginFieldType::fields` declares each `DerivedField`: a `suffix`
  (`[a-z0-9_]+`, at most 32 bytes), a `kind` (`INT`, `DATE` with a `unit`, `STRING`, `TEXT`
  with an optional registered `analyzer`, `GEO_POINT`), `multi`, `index` (the term index
  for STRING and TEXT, the points index for the others), `column` and `opaque`. They are
  compiled as built-in fields named `<field>$<suffix>`: hidden from catalogs, default
  retrieval and field patterns, never document keys (a field name cannot contain `$`), but
  queried, sorted, faceted, used in expressions (`pt$a:>=2`, `sort: "pt$b asc"`) and
  retrieved by that name. Opaque bytes go in a column-only STRING (`index = false`), like a
  collation key; mark it `opaque` and retrieving it by name fails, pointing at the field.
  The authored field itself only retrieves: searching, sorting or faceting it fails with a
  message naming its derived fields.
- **Multi-valued derived fields are independent sets.** Each multi INT, DATE or STRING
  field keeps a sorted set of its document's values (repeats dropped), TEXT and GEO_POINT
  keep theirs in order, and nothing ties the i-th value of one derived field to the i-th
  of another. A query on `ps$a` and `ps$b` matches a document whose pairs have those
  values in any combination, and `project()` sees each field's set, not the order or
  repeats `index()` emitted. Keep whatever must stay together in one value (the example's
  `key` and `packed`).
- **Ingest.** `index(value, sink)` receives the raw value of the document's field, whatever
  arm it is (object, array, string, number; null is no value and skips the call). It emits
  per derived field, addressed by its index in `fields`: `string()` (a term and/or column
  value), `text()` (analyzed by the field's analyzer), `token(term, position)` (an analyzed
  token, positions non-decreasing), `integer()` (INT, or DATE in its unit) and `point()`.
  A single-valued derived field takes one value per document. The values reach the index
  only after `index` returns, so a throw leaves nothing behind: `std::invalid_argument`
  (or `DocumentError`, whose code the error keeps) rejects the document with "Field 'pt':
  cannot read pair value: ...", or with `ignore_malformed` skips the field and indexes the
  rest. Any other exception (a misuse of the sink, a bug) fails the document with "field
  type 'x' failed to index the value", whatever `ignore_malformed` says. The sink applies
  the built-in limits as values arrive (one value for a single-valued field, a point's
  range), so they are malformed values too. A STRING or TEXT term over 255 bytes is not
  an error: it is fitted (a prefix plus a hash, `long_terms: hash128`), as a built-in
  string field's is, and a query for the same value fits it the same way.
- **Retrieval.** When `projects()` is true, retrieving the field (explicitly, by an empty
  field list, or by a pattern) loads the derived fields' columns and calls `project` with
  a document's values (`DerivedValues::get(i)`: an int64, a string, or an `ArrInt` /
  `ArrStr` when multi; TEXT and GEO_POINT fields and fields without a column are absent).
  It is called only when some value is present and builds `out` in `mr`. An indexed
  STRING's column holds the indexed term, so a value over 255 bytes comes back fitted;
  `DerivedValues::exact(i)` is false when it may be. Decode from column-only fields, which
  hold their bytes exactly, and treat bytes as untrusted (check their length). A type that
  does not project is not retrievable as a whole; its non-opaque derived fields still are.
- **Index sort.** `sortProxy()` returns the suffix of a derived field that stands in for
  the authored field when a schema's `index_sort` names it, so
  `{"index_sort": {"field": "shape", "order": "hilbert"}}` sorts merges by `shape$<suffix>`
  (a centroid GEO_POINT, for example). The derived field must suit the order: a GEO_POINT
  column for `hilbert`, an INT or DATE column for `asc` and `desc`. The default (empty)
  refuses the field as an index sort.
- **Persistence.** The authored definition is in the persisted schema, and its canonical
  form (the name, the params, sorted, and `version()` when not 0) is part of every derived
  field's signature (`plugin_type`). A changed definition is reported as a new
  representation of the derived fields in the resolved schema; segments written before
  keep their derived data, which is still read under the new definition, so a type must
  keep reading what older params wrote or the collection must be reindexed. Two hooks keep
  the signature honest: `canonicalParams()` returns the effective params as a JSON object
  (defaults filled in), so `{}` and `{"separator": ":"}` are one definition, and
  `version()` (default 0) marks a change in what `index()` writes for the same params. A
  collection whose schema names a type this binary lacks fails to load with "unknown field
  type 'x'; valid types: ...; plugin types: ..."; the node keeps serving the rest.

The example plugin registers `pair` (above, with `separator` and `multi` params and their
`canonicalParams()`; `project` decodes the opaque packed column) and `spot` (a GEO_POINT,
an analyzed TEXT and a pre-analyzed TEXT; not retrievable; its point is the sort proxy).
`FieldTypeExampleTest` covers validation, HTTP and gRPC ingest,
queries, sorts and facets on derived fields, projection, templates, malformed values,
merges with deletes, persistence, the missing-type failure and a Hilbert index sort
through the sort proxy.

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
