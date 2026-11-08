// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <stdexcept>
#include <string>

#include "CountingStorage.h"
#include "ExampleSearch.h"
#include "ExampleQuery.h"
#include "luxir/plugin/Registries.h"
#include "luxir/server/LuxirNode.h"

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

// Char filter: removes every 'x'. The base class records each deletion, so
// token offsets map back to the source.
class StripXFilter : public CharFilter {
  void filter() override {
    for (size_t i = input.find('x'); i != std::string_view::npos; i = input.find('x', i + 1)) replace(i, i + 1, "");
  }
};

// Merge policy: merges segments whose bounds on one numeric field overlap or
// touch, so a merged segment keeps a range on that field that excludes it
// from queries outside it. Segments without values for the field never merge.
class BoundsOverlapPolicy : public MergePolicy {
public:
  std::string field;
  int64_t minSegments;

  BoundsOverlapPolicy(std::string field, int64_t minSegments) : field(std::move(field)), minSegments(minSegments) {}

  std::vector<MergeSpec> findMerges(std::span<const MergeSegment> segments) const override {
    struct Range {
      int64_t min, max;
      uint64_t segId;
    };
    std::vector<Range> ranges;
    for (const auto& segment : segments) {
      if (auto bounds = segment.summary->numericBounds(field)) ranges.push_back({bounds->min, bounds->max, segment.segId});
    }
    std::ranges::sort(ranges, {}, &Range::min);
    std::vector<MergeSpec> merges;
    MergeSpec group;
    int64_t groupMax = 0;
    auto close = [&] {
      if ((int64_t)group.size() >= minSegments) merges.push_back(group);
      group.clear();
    };
    for (const auto& range : ranges) {
      if (!group.empty() && range.min > groupMax) close();
      groupMax = group.empty() ? range.max : std::max(groupMax, range.max);
      group.push_back(range.segId);
    }
    close();
    return merges;
  }
};

void noParams(const api::AnalyzerComponent& def) {
  if (!def.params.empty()) throw std::invalid_argument("'" + std::string(def.name) + "' takes no parameters");
}

// Test-only plugin that exercises the plugin framework: analysis components,
// query types, search ops, HTTP routes, the node lifecycle, and storage
// backends and layers.
namespace {

class FieldListOverlay final : public SegmentOverlayKind {
  bool fail;

public:
  explicit FieldListOverlay(bool fail) : fail(fail) {}
  std::optional<SegmentOverlayOutput> build(const SegmentOverlayBuild& request) const override {
    if (fail) throw std::runtime_error("example_field_list: failing as asked");
    std::string text;
    for (const auto& field : request.fields) text += field + "\n";
    auto file = request.dir.createFile(request.fileName(0));
    OutputStream os;
    os.setFile(&*file);
    os.write(text.data(), text.size());
    os.close();
    request.dir.finishFile(*file);
    SegmentOverlayOutput output;
    output.files.push_back(file->descriptor());
    const std::string meta = std::to_string(request.fields.size());
    output.meta.assign((const std::byte*)meta.data(), (const std::byte*)meta.data() + meta.size());
    return output;
  }
};

}  // namespace

// Public to the example's tests: what an example_field_list overlay holds.
class FieldListReader final : public AuxReader {
  std::string name;

public:
  std::string fields;
  std::string meta;
  FieldListReader(const api::AuxIndexInfo& info, std::string fields)
      : AuxReader(info.gen, info.built_core_gen), name(info.name), fields(std::move(fields)),
        meta((const char*)info.opaque_meta.data(), info.opaque_meta.size()) {}
  std::string_view getKind() const override { return "example_field_list"; }
  std::string_view getName() const override { return name; }
};

class ExamplePlugin : public Plugin {
public:
  static inline std::atomic<bool> throwOnNodeStateCreation{false};

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

    r.storage.addBackend("counting_ram", [](const StorageContext& ctx) -> std::unique_ptr<DirectoryFactory> {
      return std::make_unique<CountingRamBackend>(ctx);
    });
    r.storage.addLayer("counting", [](std::unique_ptr<DirectoryFactory> inner, const StorageContext&)
                                       -> std::unique_ptr<DirectoryFactory> {
      return std::make_unique<CountingLayer>(std::move(inner));
    });

    r.mergePolicies.add("example_bounds_overlap", [](const MergePolicyContext& ctx) {
      std::string field(ctx.params.requireString("field"));
      int64_t minSegments = ctx.params.getInt("min_segments", 2);
      if (minSegments < 2) ctx.params.fail("min_segments", "must be at least 2");
      return std::make_unique<BoundsOverlapPolicy>(std::move(field), minSegments);
    });

    // Writes a file naming the fields it covers; "fail" makes every build throw.
    r.segmentOverlays.add("example_field_list", {
      .maker = [](const SegmentOverlayContext& ctx) -> std::unique_ptr<SegmentOverlayKind> {
        return std::make_unique<FieldListOverlay>(ctx.params.getBool("fail", false));
      },
      .open = [](Directory& dir, const api::AuxIndexInfo& info) -> std::shared_ptr<AuxReader> {
        auto file = dir.openFile(info.files[0].name, /*expectSynced=*/true);
        std::string_view text = file->read();
        return std::make_shared<FieldListReader>(info, std::string(text));
      },
    });

    r.query.add("doc_ids", ::example::docIdsDef());
    r.query.add("field_value", ::example::fieldValueDef());
    r.query.add("field_value_alias", ::example::fieldValueDef());
    r.query.add("unscoped_value", ::example::unscopedDef());
    r.searchOps.add("sample_docs", ::example::makeSampleDocs);
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
