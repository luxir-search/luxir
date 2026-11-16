// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <array>
#include <cmath>
#include <tbb/global_control.h>

#include "bench/luxir_bench.h"
#include "luxir/value/ValueExprParser.h"
#include "luxir/schema/ValCoerce.h"
#include "luxir/value/ValueFunctionSupport.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/QueryBuild.h"
#include "test/TestUtils.h"

using namespace luxir;
using namespace luxir::test;

namespace {

struct ValueFixture {
  static constexpr int32_t DOCS = 2048;
  CollectionHelper helper{"value_expression_bm"};
  std::shared_ptr<IndexReader> reader;

  ValueFixture() {
    helper.clear();
    auto writer = helper.getIndexWriter();
    auto& inverter = writer->obtainInverter();
    auto& column = inverter.getIndexHandler("x_d");
    auto& array = inverter.getIndexHandler("xs_ds");
    for (int32_t doc = 0; doc < DOCS; doc++) {
      inverter.startDoc();
      double value = 1.0 + (double)doc / 1024;
      column.index(inverter, coerce::scalarVal(value));
      std::array<double, 2> values{value, value + 1};
      array.index(inverter, coerce::scalarVal(api::ArrDouble{values}));
      inverter.finishDoc();
    }
    writer->releaseInverter(inverter, true);
    helper.commit();
    reader = writer->snapshots.readers.getReader();
    if (reader->segments().size() != 1 || reader->segments()[0].maxDoc() != DOCS) {
      throw std::runtime_error("value benchmark fixture has an unexpected shape");
    }
  }
};

// The PR's specialized unary dispatch, measured against the core opcode
// dispatch with identical operations and operands. Only the root point kernel
// changes; this is an implementation experiment, not a public function.
template <double (*Fn)(double)>
ValueResult specializedUnary(BoundValueProgram& program, const ValueNode& node,
                              int32_t docid, float score) {
  ValueResult input = program.evalNode(node.children[0], docid, score);
  if (!input.valid) return ValueResult::missing(node.type);
  if (valueArray(input.type)) return ValueResult::arrayValue(
      node.type, valuefn::nodeIndex(program, node), docid, score, input.array.size);
  double value = Fn(valuefn::numberOf(input));
  if (!std::isfinite(value)) valuefn::runtimeInvalid(node, docid, "produced NaN or infinity");
  return ValueResult::floating(value);
}

void BM_ValuePoint(benchmark::State& state, const char* expression,
                    ValueFunction::EvalPoint alternative = nullptr, bool batch = false) {
  ValueFixture data;
  tbb::global_control serial{tbb::global_control::max_allowed_parallelism, 1};
  google::protobuf::Arena arena;
  ValueExprOptions options{data.helper.collection().getSchema().get(), {}};
  auto* program = ValueExprParser(options, arena).parse(expression);
  ValueFunction function;
  if (alternative != nullptr) {
    function = *program->root().function;
    function.evalPoint = alternative;
    program->nodes[program->rootNode].function = &function;
  }
  MemPool pool;
  auto* bound = program->bind(pool, data.reader->segments()[0], program->resolveFields(pool, *data.reader));
  if (!bound->evalPoint(0, 0).valid) {
    state.SkipWithError("value benchmark unexpectedly produced a missing result");
    return;
  }
  std::array<int32_t, 64> docs;
  std::array<ValueResult, 64> results;
  for (auto _ : state) {
    if (batch) {
      for (int32_t start = 0; start < ValueFixture::DOCS; start += (int32_t)docs.size()) {
        for (size_t i = 0; i < docs.size(); i++) docs[i] = start + (int32_t)i;
        bound->evalBatch(docs, {}, results);
        benchmark::DoNotOptimize(results);
      }
    } else {
      for (int32_t doc = 0; doc < ValueFixture::DOCS; doc++) {
        auto result = bound->evalPoint(doc, 0);
        benchmark::DoNotOptimize(result);
      }
    }
  }
  state.SetItemsProcessed(state.iterations() * ValueFixture::DOCS);
}

void BM_ValueRequest(benchmark::State& state, const char* expression, bool rescore) {
  ValueFixture data;
  for (auto _ : state) {
    auto req = localReq(data.helper.getSearchEngine());
    auto& top = req->collection("value_expression_bm").topDocs("q").limit(10).countLimit(0);
    if (rescore) top.exprQuery(std::string("rescore(*:*, expr=") + expression + ")");
    else {
      top.allQuery();
      qb::sort(top, expression, qb::ASC);
    }
    req->execute(false);
    if (!req->ok()) { state.SkipWithError(std::string(req->errorMsg()).c_str()); break; }
    benchmark::DoNotOptimize(req->docList());
  }
  state.SetItemsProcessed(state.iterations() * ValueFixture::DOCS);
}

LUXIR_BENCHMARK_CAPTURE(BM_ValuePoint, sqrt, "sqrt(x_d)");
LUXIR_BENCHMARK_CAPTURE(BM_ValuePoint, sqrt_specialized, "sqrt(x_d)", specializedUnary<std::sqrt>);
LUXIR_BENCHMARK_CAPTURE(BM_ValuePoint, log, "log(x_d)");
LUXIR_BENCHMARK_CAPTURE(BM_ValuePoint, log_specialized, "log(x_d)", specializedUnary<std::log>);
LUXIR_BENCHMARK_CAPTURE(BM_ValuePoint, floor, "floor(x_d)");
LUXIR_BENCHMARK_CAPTURE(BM_ValuePoint, floor_specialized, "floor(x_d)", specializedUnary<std::floor>);
LUXIR_BENCHMARK_CAPTURE(BM_ValuePoint, exp, "exp(x_d)");
LUXIR_BENCHMARK_CAPTURE(BM_ValuePoint, exp_specialized, "exp(x_d)", specializedUnary<std::exp>);
LUXIR_BENCHMARK_CAPTURE(BM_ValuePoint, ceil, "ceil(x_d)");
LUXIR_BENCHMARK_CAPTURE(BM_ValuePoint, arithmetic, "(x_d * 1.1 + 2) * 0.9 + x_d");
LUXIR_BENCHMARK_CAPTURE(BM_ValuePoint, shared, "let(v, sqrt(x_d), v * v + v)");
LUXIR_BENCHMARK_CAPTURE(BM_ValuePoint, array, "sum(sqrt(xs_ds))");
LUXIR_BENCHMARK_CAPTURE(BM_ValuePoint, batch_sqrt, "sqrt(x_d)", nullptr, true);
LUXIR_BENCHMARK_CAPTURE(BM_ValuePoint, batch_floor, "floor(x_d)", nullptr, true);
LUXIR_BENCHMARK_CAPTURE(BM_ValueRequest, sort_sqrt, "sqrt(x_d)", false);
LUXIR_BENCHMARK_CAPTURE(BM_ValueRequest, sort_floor, "floor(x_d)", false);
LUXIR_BENCHMARK_CAPTURE(BM_ValueRequest, rescore_sqrt, "sqrt(x_d)", true);
LUXIR_BENCHMARK_CAPTURE(BM_ValueRequest, rescore_floor, "floor(x_d)", true);

} // namespace
