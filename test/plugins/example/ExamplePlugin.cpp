// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <cstring>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>

#include <glaze/glaze.hpp>

#include "CountingStorage.h"
#include "ExampleSearch.h"
#include "ExampleSort.h"
#include "luxir/api/build.h"
#include "ExampleQuery.h"
#include "luxir/plugin/Registries.h"
#include "luxir/server/LuxirNode.h"
#include "luxir/store/DelegatingDirectory.h"
#include "luxir/util/ApiError.h"

namespace luxir::example {

namespace {

// Filter: reverses the bytes of every token (ASCII only, which is all the
// tests feed it). Rewrites into its own buffer per the token borrow contract.
class ReverseFilter : public TokenFilter {
  std::string buf;

public:
  explicit ReverseFilter(std::unique_ptr<TokenStream> source) : TokenFilter(std::move(source)) {}

  bool incrementToken() override {
    if (!source().incrementToken()) return false;
    buf.assign(token.text.rbegin(), token.text.rend());
    token.text = buf;
    return true;
  }

  void normalizeTerm(std::string& term) override {
    TokenFilter::normalizeTerm(term);
    std::reverse(term.begin(), term.end());
  }
};

// Filter: sets positionLength to 2 on tokens that start with 'x', as a filter
// emitting a two-word synonym would.
class Span2Filter : public TokenFilter {
public:
  explicit Span2Filter(std::unique_ptr<TokenStream> source) : TokenFilter(std::move(source)) {}

  bool incrementToken() override {
    if (!source().incrementToken()) return false;
    if (!token.text.empty() && token.text[0] == 'x') token.positionLength = 2;
    return true;
  }
};

// Filter: rewrites every token to "text|type|script|positionLength", what a
// downstream plugin filter can read of the attributes upstream stages set.
class DescribeFilter : public TokenFilter {
  std::string buf;

public:
  explicit DescribeFilter(std::unique_ptr<TokenStream> source) : TokenFilter(std::move(source)) {}

  bool incrementToken() override {
    if (!source().incrementToken()) return false;
    buf.assign(token.text);
    buf += '|';
    buf += tokenTypeName(token.type);
    buf += '|';
    buf += scriptName(token.script);
    buf += '|';
    buf += std::to_string(token.positionLength);
    token.text = buf;
    return true;
  }
};

// Char filter: removes every 'x'. The base class records each deletion, so
// token offsets map back to the source.
class StripXFilter : public CharFilter {
  void filter() override {
    for (size_t i = input.find('x'); i != std::string_view::npos; i = input.find('x', i + 1)) replace(i, i + 1, "");
  }
};

// Field type "pair": {"a": <int>, "b": <int>} (with params {"multi": true},
// an array of them). Derives two INT columns, a keyword "<a><separator><b>"
// and the pair packed into 16 bytes in a column-only STRING, which is what
// project() decodes back into the object.
class PairType : public PluginFieldType {
public:
  enum Field { A, B, KEY, PACKED };
  std::string separator;
  bool multi;

  PairType(std::string separator, bool multi) : separator(std::move(separator)), multi(multi) {
    fields = {{.suffix = "a", .kind = DerivedField::Kind::INT, .multi = multi},
              {.suffix = "b", .kind = DerivedField::Kind::INT, .multi = multi},
              {.suffix = "key", .kind = DerivedField::Kind::STRING, .multi = multi},
              {.suffix = "packed", .kind = DerivedField::Kind::STRING, .multi = multi, .index = false,
               .opaque = true}};
  }

  // The defaults filled in, so {} and {"separator": ":"} are one definition.
  std::string canonicalParams() const override {
    glz::generic_sorted_u64 params;
    params["separator"] = separator;
    params["multi"] = multi;
    return glz::write_json(params).value_or("");
  }

  void index(const api::Val& value, DerivedSink& sink) const override {
    if (auto* list = std::get_if<api::ArrVal>(&value.kind); list && multi) {
      for (const auto& element : list->v) indexPair(element, sink);
    } else {
      indexPair(value, sink);
    }
  }

  bool projects() const override { return true; }

  void project(const DerivedValues& values, api::Val& out, std::pmr::memory_resource& mr) const override {
    const api::Val* packed = values.get(PACKED);
    if (packed == nullptr) return;
    if (!multi) {
      out = pairValue(std::get<std::string_view>(packed->kind), mr);
      return;
    }
    // A multi pair keeps a sorted set of packed values, so pairs come back in that order.
    const auto& all = std::get<api::ArrStr>(packed->kind).v;
    api::ArrVal list;
    auto* items = api::build::allocArray(list.v, all.size(), mr);
    for (size_t i = 0; i < all.size(); i++) items[i] = pairValue(all[i], mr);
    out.kind = list;
  }

private:
  static int64_t member(const api::Map& map, std::string_view name) {
    const auto* found = map.fields.find(name);
    if (found == nullptr) throw std::invalid_argument("'" + std::string(name) + "' is required");
    if (auto* i = std::get_if<int64_t>(&(*found)->kind)) return *i;
    throw std::invalid_argument("'" + std::string(name) + "' must be an integer");
  }

  void indexPair(const api::Val& value, DerivedSink& sink) const {
    auto* map = std::get_if<api::Map>(&value.kind);
    if (map == nullptr) throw std::invalid_argument("expected an object {\"a\": <int>, \"b\": <int>}");
    for (const auto& entry : map->fields) {
      if (entry.first != "a" && entry.first != "b") {
        throw std::invalid_argument("unknown member '" + std::string(entry.first) + "'");
      }
    }
    int64_t a = member(*map, "a"), b = member(*map, "b");
    sink.integer(A, a);
    sink.integer(B, b);
    sink.string(KEY, std::to_string(a) + separator + std::to_string(b));
    char bytes[16];
    std::memcpy(bytes, &a, 8);
    std::memcpy(bytes + 8, &b, 8);
    sink.string(PACKED, std::string_view(bytes, 16));
  }

  // Null for bytes this type did not write.
  static api::Val pairValue(std::string_view bytes, std::pmr::memory_resource& mr) {
    if (bytes.size() != 16) return {};
    int64_t a, b;
    std::memcpy(&a, bytes.data(), 8);
    std::memcpy(&b, bytes.data() + 8, 8);
    api::Map map;
    auto* va = api::build::mapSlot<api::Val>(map.fields, 2, "a", mr);
    va->kind = a;
    api::build::mapSlot<api::Val>(map.fields, 2, "b", mr)->kind = b;
    api::Val out;
    out.kind = map;
    return out;
  }
};

// Field type "spot": {"label": <text>, "lat": <num>, "lon": <num>, "tokens":
// [[<term>, <position>], ...]}. Derives a geo point, the analyzed label, and
// pre-analyzed tokens (a TEXT field): the given tokens, else the label's
// letters at their offsets. Not retrievable. Given positions go to the sink
// unchecked, so decreasing ones fail the document as a type fault.
class SpotType : public PluginFieldType {
public:
  enum Field { AT, LABEL, LETTERS };

  SpotType() {
    fields = {{.suffix = "at", .kind = DerivedField::Kind::GEO_POINT},
              {.suffix = "label", .kind = DerivedField::Kind::TEXT, .column = false, .analyzer = "english"},
              {.suffix = "letters", .kind = DerivedField::Kind::TEXT, .column = false}};
  }

  // index_sort {"field": <spot field>, "order": "hilbert"} sorts by the point.
  std::string_view sortProxy() const override { return "at"; }

  void index(const api::Val& value, DerivedSink& sink) const override {
    auto* map = std::get_if<api::Map>(&value.kind);
    if (map == nullptr) throw std::invalid_argument("expected an object");
    auto number = [&](std::string_view name) {
      const auto* found = map->fields.find(name);
      if (found == nullptr) throw std::invalid_argument("'" + std::string(name) + "' is required");
      if (auto* d = std::get_if<double>(&(*found)->kind)) return *d;
      if (auto* i = std::get_if<int64_t>(&(*found)->kind)) return (double)*i;
      throw std::invalid_argument("'" + std::string(name) + "' must be a number");
    };
    sink.point(AT, number("lat"), number("lon"));
    const auto* tokens = map->fields.find("tokens");
    if (tokens != nullptr) {
      const auto* list = std::get_if<api::ArrVal>(&(*tokens)->kind);
      if (list == nullptr) throw std::invalid_argument("'tokens' must be an array");
      for (const auto& token : list->v) {
        const auto* pair = std::get_if<api::ArrVal>(&token.kind);
        const std::string_view* term = pair && pair->v.size() == 2
            ? std::get_if<std::string_view>(&pair->v[0].kind) : nullptr;
        const int64_t* position = term ? std::get_if<int64_t>(&pair->v[1].kind) : nullptr;
        if (position == nullptr) throw std::invalid_argument("each token must be [<term>, <position>]");
        sink.token(LETTERS, *term, (int32_t)*position);
      }
    }
    if (const auto* label = map->fields.find("label")) {
      const auto* text = std::get_if<std::string_view>(&(*label)->kind);
      if (text == nullptr) throw DocumentError("'label' must be a string", "invalid_label");
      sink.text(LABEL, *text);
      for (size_t i = 0; tokens == nullptr && i < text->size(); i++) {
        if ((*text)[i] != ' ') sink.token(LETTERS, text->substr(i, 1), (int32_t)i);
      }
    }
  }
};

// Collation: ASCII case-insensitive order. strength=primary keys are the
// lowercased value, so "a" and "A" tie; strength=tertiary (default) appends the
// case of each letter, lowercase first, as ICU's default does.
class CaseCollator : public Collator {
  bool tertiary;

public:
  explicit CaseCollator(bool tertiary) : tertiary(tertiary) {}

  void appendKey(std::string_view value, std::string& key) override {
    for (char c : value) key.push_back((c >= 'A' && c <= 'Z') ? (char)(c + ('a' - 'A')) : c);
    if (!tertiary) return;
    key.push_back('\0');
    for (char c : value) key.push_back((c >= 'A' && c <= 'Z') ? '\1' : '\0');
  }
};

void noParams(const api::AnalyzerComponent& def) {
  if (!def.params.empty()) throw std::invalid_argument("'" + std::string(def.name) + "' takes no parameters");
}

std::mutex countsMutex;
std::map<std::string, uint64_t>& counts() {
  static std::map<std::string, uint64_t> instance;
  return instance;
}
std::string key(std::string_view source, std::string_view op, std::string_view collection) {
  return std::string(source) + "/" + std::string(op) + "/" + std::string(collection);
}

// Counts the files opened and created through it, under its collection's label.
class CountingDirectory : public DelegatingDirectory {
  std::string collection;

public:
  CountingDirectory(std::shared_ptr<Directory> inner, std::string collection)
      : DelegatingDirectory(std::move(inner)), collection(std::move(collection)) {}

  std::shared_ptr<InputFile> openFile(std::string_view name, bool expectSynced = false) override {
    CountingStorage::add("layer", "open", collection);
    return delegate->openFile(name, expectSynced);
  }
  std::unique_ptr<File> createFile(std::string_view name) override {
    CountingStorage::add("layer", "create", collection);
    return delegate->createFile(name);
  }
  std::unique_ptr<File> createFile(std::string_view name, FileCreateOptions options) override {
    CountingStorage::add("layer", "create", collection);
    return delegate->createFile(name, std::move(options));
  }
};

// The counting layer: wraps every directory it hands out.
class CountingLayer : public DelegatingDirFactory {
public:
  using DelegatingDirFactory::DelegatingDirFactory;

  std::shared_ptr<Directory> container(const CollectionId& id, bool create) override {
    return std::make_shared<CountingDirectory>(delegate->container(id, create), id.label());
  }
  std::shared_ptr<Directory> incarnation(const CollectionId& id, std::string_view incarnation, bool create) override {
    return std::make_shared<CountingDirectory>(delegate->incarnation(id, incarnation, create), id.label());
  }
  std::string describe() const override { return "counting(" + delegate->describe() + ")"; }
};

// The counting_ram backend: RAM storage that counts directory opens and creates.
class CountingRamBackend : public DelegatingDirFactory {
  static void count(const CollectionId& id, bool create) {
    CountingStorage::add("backend", create ? "create" : "open", id.label());
  }

public:
  explicit CountingRamBackend(const StorageContext& ctx)
      : DelegatingDirFactory(std::make_unique<RAMDirFactory>(0, ctx.reclaim)) {}

  std::shared_ptr<Directory> container(const CollectionId& id, bool create) override {
    count(id, create);
    return delegate->container(id, create);
  }
  std::shared_ptr<Directory> incarnation(const CollectionId& id, std::string_view incarnation, bool create) override {
    count(id, create);
    return delegate->incarnation(id, incarnation, create);
  }
  std::string describe() const override { return "counting_ram"; }
};

}  // namespace

void CountingStorage::reset() {
  std::lock_guard lock(countsMutex);
  counts().clear();
}

void CountingStorage::add(std::string_view source, std::string_view op, std::string_view collection) {
  std::lock_guard lock(countsMutex);
  counts()[key(source, op, collection)]++;
  if (!collection.empty()) counts()[key(source, op, "")]++;
}

uint64_t CountingStorage::count(std::string_view source, std::string_view op, std::string_view collection) {
  std::lock_guard lock(countsMutex);
  auto it = counts().find(key(source, op, collection));
  return it == counts().end() ? 0 : it->second;
}

namespace {

// Test-only plugin that exercises the plugin framework: analysis components,
// collations, query types, search ops, HTTP routes, the node lifecycle, and
// storage backends and layers.
class ExamplePlugin : public Plugin {
public:
  static inline std::atomic<bool> throwOnNodeStateCreation{false};

  static void registerFilter(Registries& r, std::string name,
                             std::unique_ptr<TokenStream> (*make)(std::unique_ptr<TokenStream>)) {
    r.analysis.addFilter(name, [name, make](const api::AnalyzerComponent& def) {
      noParams(def);
      struct Factory : TokenFilterFactory {
        std::unique_ptr<TokenStream> (*make)(std::unique_ptr<TokenStream>);
        std::unique_ptr<TokenStream> create(std::unique_ptr<TokenStream> source) const override {
          return make(std::move(source));
        }
      };
      auto factory = std::make_unique<Factory>();
      factory->name = name;
      factory->make = make;
      return std::unique_ptr<TokenFilterFactory>(std::move(factory));
    });
  }

  static void registerCollation(Registries& r) {
    r.collation.add("example_ci", [](const api::AnalyzerComponent& def) {
      bool tertiary = true;
      for (const auto& [key, value] : def.params) {
        auto* text = std::get_if<std::string_view>(&value->kind);
        if (key != "strength") {
          throw std::invalid_argument("collation 'example_ci': unknown parameter '" + std::string(key) +
                                      "'; valid parameters: strength");
        }
        if (!text || (*text != "primary" && *text != "tertiary")) {
          throw std::invalid_argument("collation 'example_ci': parameter 'strength' must be \"primary\" or \"tertiary\"");
        }
        tertiary = *text == "tertiary";
      }
      struct Factory : CollatorFactory {
        bool tertiary = true;
        std::unique_ptr<Collator> create() const override { return std::make_unique<CaseCollator>(tertiary); }
      };
      auto factory = std::make_unique<Factory>();
      factory->tertiary = tertiary;
      return std::unique_ptr<const CollatorFactory>(std::move(factory));
    });
  }

  static void registerFieldTypes(Registries& r) {
    r.fieldTypes.add("pair", [](const ParamReader& params) -> std::unique_ptr<const PluginFieldType> {
      std::string separator(params.getString("separator", ":"));
      if (separator.empty()) params.fail("separator", "must not be empty");
      return std::make_unique<PairType>(std::move(separator), params.getBool("multi", false));
    });
    r.fieldTypes.add("spot", [](const ParamReader&) -> std::unique_ptr<const PluginFieldType> {
      return std::make_unique<SpotType>();
    });
  }

  PluginInfo info() const override { return {"example", "1.0", "exercises the plugin framework"}; }

  void registerComponents(Registries& r) override {
    r.analysis.addFilter("example_reverse", [](const api::AnalyzerComponent& def) {
      noParams(def);
      struct Factory : TokenFilterFactory {
        std::unique_ptr<TokenStream> create(std::unique_ptr<TokenStream> source) const override {
          return std::make_unique<ReverseFilter>(std::move(source));
        }
      };
      auto factory = std::make_unique<Factory>();
      factory->name = "example_reverse";
      factory->normalizing = true;
      return std::unique_ptr<TokenFilterFactory>(std::move(factory));
    });
    r.analysis.addCharFilter("strip_x", [](const api::AnalyzerComponent& def) {
      noParams(def);
      struct Factory : CharFilterFactory {
        std::unique_ptr<CharFilter> create() const override { return std::make_unique<StripXFilter>(); }
      };
      auto factory = std::make_unique<Factory>();
      factory->name = "strip_x";
      factory->normalizing = true;
      return std::unique_ptr<const CharFilterFactory>(std::move(factory));
    });

    api::AnalyzerComponent whitespace, stripX, lowercase, reverse;
    whitespace.name = "whitespace";
    stripX.name = "strip_x";
    lowercase.name = "lowercase";
    reverse.name = "example_reverse";
    api::AnalyzerDef example;
    example.tokenizer = whitespace;
    const api::AnalyzerComponent charFilters[] = {stripX};
    const api::AnalyzerComponent filters[] = {lowercase, reverse};
    example.char_filters = charFilters;
    example.filters = filters;
    r.analysis.addAnalyzer("example", example);

    registerFilter(r, "span2", [](std::unique_ptr<TokenStream> source) -> std::unique_ptr<TokenStream> {
      return std::make_unique<Span2Filter>(std::move(source));
    });
    registerFilter(r, "describe", [](std::unique_ptr<TokenStream> source) -> std::unique_ptr<TokenStream> {
      return std::make_unique<DescribeFilter>(std::move(source));
    });
    registerCollation(r);
    registerFieldTypes(r);

    r.storage.addBackend("counting_ram", [](const StorageContext& ctx) -> std::unique_ptr<DirectoryFactory> {
      return std::make_unique<CountingRamBackend>(ctx);
    });
    r.storage.addLayer("counting", [](std::unique_ptr<DirectoryFactory> inner, const StorageContext&)
                                       -> std::unique_ptr<DirectoryFactory> {
      return std::make_unique<CountingLayer>(std::move(inner));
    });

    r.query.add("doc_ids", ::example::docIdsDef());
    r.query.add("field_value", ::example::fieldValueDef());
    r.query.add("field_value_alias", ::example::fieldValueDef());
    r.query.add("unscoped_value", ::example::unscopedDef());
    r.searchOps.add("sample_docs", ::example::makeSampleDocs);
    r.sortKinds.add("abs_diff", ::example::makeAbsDiff);
    r.http.add("GET", "/_plugins/example/ping", [](HttpPluginRequest& request, HttpPluginResponse& response) {
      request.node.pluginState<::example::NodeState>("example")->routes++;
      const std::string* echo = request.queryParam("echo");
      response.body = R"({"pong":")" + (echo ? *echo : std::string()) + R"("})";
    });
    // A collection route answered by a search: POST {"ids": [...]} runs sample_docs.
    r.http.add("POST", "/collections/{collection}/_sample", [](HttpPluginRequest& request, HttpPluginResponse& response) {
      response.searchCollection = *request.pathParam("collection");
      response.searchBody = R"({"ops":{"sample":{"sample_docs":)" + std::string(request.body) + "}}}";
    });
  }
  std::unique_ptr<luxir::PluginNodeState> createNodeState(luxir::LuxirNode& node) override {
    if (throwOnNodeStateCreation.load()) throw std::runtime_error("example node state creation failed");
    return std::make_unique<::example::NodeState>(node);
  }
};

}  // namespace

}  // namespace luxir::example

// For the node-construction failure test.
void exampleThrowOnNodeStateCreation(bool value) { luxir::example::ExamplePlugin::throwOnNodeStateCreation.store(value); }

LUXIR_PLUGIN(example, luxir::example::ExamplePlugin)
