// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <charconv>
#include <cstdlib>

#include "bench/luxir_bench.h"
#include "luxir/query/BooleanWeight.h"
#include "luxir/query/NumericPredicateQuery.h"
#include "luxir/query/TermQuery.h"
#include "luxir/reader/SkipStats.h"
#include "test/CollectionHelper.h"
#include "test/ConjunctionPlanControl.h"
#include "test/SchemaBuilder.h"

using namespace luxir;
using namespace luxir::test;

namespace {

using Consumption = Query::MembershipConsumption;
using Use = Query::ExecutionUse;
constexpr int32_t WINDOW = DocsEnumMeta::L1_DOCS;

std::string setting(const char* name, const char* fallback) {
  const char* value = std::getenv(name);
  return value == nullptr ? fallback : value;
}

int32_t number(const char* name, int32_t fallback) {
  const char* value = std::getenv(name);
  return value == nullptr ? fallback : std::atoi(value);
}

uint64_t sample(int64_t doc, uint64_t seed) {
  uint64_t z = seed + ((uint64_t)doc + 1) * 0x9e3779b97f4a7c15ULL;
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
  return (z ^ (z >> 31)) & 0x7fffffffffffffffULL;
}

enum class Field { TERM, A, B, MISSING, MULTI, POINTS, CLUSTER, MIXED, ADMIT };

struct Clause {
  std::string name;
  Field field = Field::TERM;
  int64_t lo = 0;
  int64_t hi = 0;
  int32_t percent = 0;
  uint64_t seed = 1;
  Query* query = nullptr;

  Clause(MemPool& pool, std::string text) {
    auto colon = text.find(':');
    name = text.substr(0, colon);
    if (colon == std::string::npos) {
      percent = std::atoi(name.c_str() + 1);
      seed = name[0] == 't' ? 1 : name[0] == 'u' ? 2 : name[0] == 'v' ? 3 : 4;
      query = pool.make<TermQuery>("body_w", name);
      return;
    }
    auto end = text.find(':', colon + 1);
    lo = std::stoll(text.substr(colon + 1, end - colon - 1));
    hi = std::stoll(text.substr(end + 1));
    field = name == "a_i" ? Field::A : name == "b_i" ? Field::B
        : name == "missing_i" ? Field::MISSING : name == "multi_is" ? Field::MULTI
        : name == "points_i" ? Field::POINTS : name == "cluster_i" ? Field::CLUSTER
        : name == "mixed_i" ? Field::MIXED : Field::ADMIT;
    query = pool.make<NumericPredicateQuery>(name, lo, hi);
  }
};

struct Fixture {
  int32_t size = number("LUXIR_CALIB_DOCS", 10000000);
  int32_t outside = number("LUXIR_CALIB_OUTSIDE", -1); // basis points of leaves
  bool fragmented = number("LUXIR_CALIB_FRAGMENTED", 0) != 0;
  bool admission;
  CollectionHelper helper;
  std::shared_ptr<IndexReader> reader;

  explicit Fixture(bool admission)
      : admission(admission), helper("conjunction_calib_v2_" + std::to_string(size)
          + (admission ? "_admit_" + std::to_string(outside + 1) + "_"
              + std::to_string(fragmented) : "_base")) {
    std::array<int32_t, 1> shape{size};
    if (!helper.indexMatchesShape(shape)) build();
    auto writer = helper.getIndexWriter();
    writer->snapshots.readers.filterCache = std::make_shared<FilterCache>(
        FilterCacheConfig{.maxBytes = 0});
    reader = std::make_shared<IndexReader>(writer->dir);
    if (reader->segments().size() != 1 || reader->segments()[0].maxDoc() != size) {
      throw std::runtime_error("calibration fixture is not one complete segment");
    }
  }

  int64_t value(Field field, int32_t doc, int32_t ordinal = 0) const {
    int64_t a = (int64_t)(sample(doc, 0) % 100000);
    switch (field) {
      case Field::B: return (int64_t)(sample(doc, 7) % 100000);
      case Field::CLUSTER: return (int64_t)doc * 100000 / size;
      case Field::MIXED: return doc < size / 4 ? 0 : doc >= size * 3 / 4 ? 100000 : a;
      case Field::MULTI: return ordinal == 0 ? a : (int64_t)(sample(doc, ordinal + 6) % 100000);
      case Field::ADMIT: {
        if (outside < 0) return a;
        int64_t leaf = doc / 512;
        int64_t leaves = (size + 511) / 512;
        bool reject = fragmented ? (leaf * 7919 % 10000) < outside
                                 : leaf < leaves * outside / 10000;
        if (reject) return 1000 + a % 99000;
        return doc % 512 == 0 ? 0 : a;
      }
      default: return a;
    }
  }

  bool matches(const Clause& clause, int32_t doc) const {
    if (clause.field == Field::TERM) return sample(doc, clause.seed) % 100 < (uint64_t)clause.percent;
    if (clause.field == Field::MISSING && doc % 3 == 0) return false;
    for (int32_t i = 0; i < (clause.field == Field::MULTI ? 3 : 1); i++) {
      int64_t v = value(clause.field, doc, i);
      if (v >= clause.lo && v <= clause.hi) return true;
    }
    return false;
  }

  void build() {
    if (helper.getIndexWriter()->snapshots.readers.getReader()->maxDoc() != 0) {
      throw std::runtime_error("stale calibration fixture; use a fresh data directory");
    }
    SchemaBuilder schema;
    auto& points = schema.field("points_i");
    points.type = api::FieldDef::FieldClass::INT;
    points.index = api::FieldDef::IndexMode::RANGE;
    schema.set(helper.collection());
    auto writer = helper.getIndexWriter();
    auto& inverter = writer->obtainInverter();
    auto& body = inverter.getIndexHandler("body_w");
    std::vector<std::pair<Field, Inverter::InputHandler*>> fields;
    if (admission) {
      fields.emplace_back(Field::ADMIT, &inverter.getIndexHandler("admit_i"));
    } else {
      for (const auto& [field, name] : std::initializer_list<std::pair<Field, const char*>>{
          {Field::A, "a_i"}, {Field::B, "b_i"}, {Field::MISSING, "missing_i"},
          {Field::MULTI, "multi_is"}, {Field::POINTS, "points_i"},
          {Field::CLUSTER, "cluster_i"}, {Field::MIXED, "mixed_i"}}) {
        fields.emplace_back(field, &inverter.getIndexHandler(name));
      }
    }
    for (int32_t doc = 0; doc < size; doc++) {
      std::string text = "pad";
      uint64_t t = sample(doc, 1) % 100;
      for (int32_t percent = 10; percent <= 90; percent += 10) {
        if (t < (uint64_t)percent) text += " t" + std::to_string(percent);
      }
      if (sample(doc, 2) % 100 < 50) text += " u50";
      if (sample(doc, 3) % 100 < 90) text += " v90";
      if (sample(doc, 4) % 100 < 90) text += " w90";
      inverter.startDoc();
      body.index(inverter, text);
      for (auto [field, handler] : fields) {
        if (field == Field::MISSING && doc % 3 == 0) continue;
        if (field == Field::MULTI) {
          std::array<int64_t, 3> values{value(field, doc), value(field, doc, 1), value(field, doc, 2)};
          handler->index(inverter, std::span<const int64_t>(values));
        } else {
          handler->index(inverter, value(field, doc));
        }
      }
      inverter.finishDoc();
    }
    writer->releaseInverter(inverter, true);
    helper.commit();
  }
};

void statistics(benchmark::State& state) {
  state.counters["scan"] = (double)SkipStats::numericPredicateScanArms;
  state.counters["zone"] = (double)SkipStats::numericPredicateZoneArms;
  state.counters["verify"] = (double)SkipStats::numericPredicateSparseVerifyArms;
  state.counters["points"] = (double)SkipStats::numericPredicatePointsArms;
  state.counters["complement"] = (double)SkipStats::numericPredicateComplementArms;
  state.counters["windows"] = (double)SkipStats::conjDenseCountWindows;
  state.counters["posting_blocks"] = (double)SkipStats::docBlocksDecoded;
  state.counters["fill_docs"] = (double)SkipStats::countBulkFillDocs;
  state.counters["fill_calls"] = (double)SkipStats::countBulkFillCalls;
  state.counters["fill_word_blocks"] = (double)SkipStats::countBulkFillWordBlocks;
  state.counters["advances"] = (double)SkipStats::advanceCalls;
  state.counters["l0_steps"] = (double)SkipStats::l0HeaderSteps;
  state.counters["l1_steps"] = (double)SkipStats::l1GroupSteps;
}

void calibration(benchmark::State& state, const char* namedQuery = nullptr) {
  if (std::getenv("LUXIR_CALIB_LEAD") != nullptr) {
    throw std::invalid_argument("LUXIR_CALIB_LEAD is obsolete; use LUXIR_CALIB_ORDER");
  }
  MemPool queries;
  std::vector<Clause> clauses;
  std::string spec = namedQuery != nullptr ? namedQuery
      : setting("LUXIR_CALIB_QUERY", "t10,a_i:0:999");
  // Leaf queries borrow the clause names, including their inline string storage.
  clauses.reserve((size_t)std::ranges::count(spec, ',') + 1);
  for (size_t start = 0; start < spec.size();) {
    auto end = spec.find(',', start);
    clauses.emplace_back(queries, spec.substr(start, end - start));
    if (end == std::string::npos) break;
    start = end + 1;
  }
  std::vector<size_t> order;
  std::string orderSpec = setting("LUXIR_CALIB_ORDER", "");
  for (size_t start = 0; start < orderSpec.size();) {
    auto end = orderSpec.find(',', start);
    if (end == std::string::npos) end = orderSpec.size();
    size_t ordinal;
    auto parsed = std::from_chars(orderSpec.data() + start, orderSpec.data() + end, ordinal);
    if (parsed.ec != std::errc{} || parsed.ptr != orderSpec.data() + end
        || (end < orderSpec.size() && end + 1 == orderSpec.size())) {
      throw std::invalid_argument("LUXIR_CALIB_ORDER must be comma-separated entry ordinals");
    }
    order.push_back(ordinal);
    start = end + 1;
  }
  if (!order.empty() && order.size() != clauses.size()) {
    throw std::invalid_argument("LUXIR_CALIB_ORDER must contain every entry ordinal");
  }
  bool admission = std::ranges::any_of(clauses, [](const auto& c) { return c.field == Field::ADMIT; });
  Fixture index(admission);
  auto& segment = index.reader->segments()[0];
  const int32_t size = index.size;
  const int32_t limit = (int32_t)((int64_t)size * number("LUXIR_CALIB_END_PERCENT", 100) / 100);
  std::string mode = setting("LUXIR_CALIB_MODE", "full");
  int32_t density = number("LUXIR_CALIB_CANDIDATES", 10000); // parts per million
  int32_t presence = number("LUXIR_CALIB_PRESENCE", -1);
  int32_t mask = number("LUXIR_CALIB_MASK", -1);
  std::vector<Consumption> consumption;
  if (mask >= 0) {
    for (size_t i = 0; i < clauses.size(); i++) consumption.push_back(
        (mask & (1 << i)) != 0 ? Consumption::CANDIDATES : Consumption::WINDOWS);
  }
  BooleanQuery::ConjunctionControlForTests control{
      order, consumption, number("LUXIR_CALIB_PROOFS", 1) != 0, {}};
  ConjunctionPlanControl forced(control);
  std::vector<Query*> required;
  for (auto& clause : clauses) required.push_back(clause.query);
  BooleanQuery conjunction{std::span(required).first(required.size() - 1), {}, {},
                           std::span(required).last(1)};
  Query& query = required.size() == 1 ? *required[0] : (Query&)conjunction;

  // Truth and kernel inputs are formula-derived, independently of the scorers.
  std::vector<uint64_t> candidates((size + 63) / 64);
  std::vector<int32_t> targets;
  std::vector<int64_t> truth(clauses.size());
  int64_t expected = 0, candidateMatches = 0, present = 0, examined = 0;
  for (int32_t doc = 0; doc < size; doc++) {
    bool match = true;
    for (size_t i = 0; i < clauses.size(); i++) {
      bool member = index.matches(clauses[i], doc);
      truth[i] += member;
      match &= member;
    }
    expected += match && doc < limit;
    if (sample(doc, 23) % 1000000 >= (uint64_t)density) continue;
    if (presence >= 0 && (doc % 3 != 0) != (presence != 0)) continue;
    candidates[(size_t)doc >> 6] |= 1ULL << (doc & 63);
    targets.push_back(doc);
    candidateMatches += index.matches(clauses[0], doc);
    bool hasValue = clauses[0].field != Field::MISSING || doc % 3 != 0;
    present += hasValue;
    if (!hasValue) continue;
    if (clauses[0].field == Field::MULTI) {
      std::array<int64_t, 3> values{index.value(Field::MULTI, doc),
          index.value(Field::MULTI, doc, 1), index.value(Field::MULTI, doc, 2)};
      std::sort(values.begin(), values.end());
      auto last = std::unique(values.begin(), values.end());
      for (auto v = values.begin(); v != last; ++v) {
        examined++;
        if (*v >= clauses[0].lo && *v <= clauses[0].hi) break;
      }
    } else examined++;
  }
  state.counters["docs"] = size;
  state.counters["limit"] = limit;
  state.counters["result"] = (double)expected;
  state.counters["candidates"] = (double)targets.size();
  state.counters["candidate_matches"] = (double)candidateMatches;
  state.counters["present"] = (double)present;
  state.counters["examined"] = (double)examined;

  auto entryStats = [&](size_t ordinal, Query::ScorerSupplier& supplier) {
    std::string p = "e" + std::to_string(ordinal) + "_";
    auto work = supplier.membershipWork(size);
    state.counters[p + "cost"] = (double)supplier.cost();
    state.counters[p + "yield"] = work.matches;
    state.counters[p + "produce"] = work.produce;
    state.counters[p + "seek"] = work.seekSteps;
    state.counters[p + "window"] = work.window;
    state.counters[p + "truth"] = (double)truth[ordinal];
  };
  for (size_t i = 0; i < clauses.size(); i++) {
    MemPool pool;
    Query::Context context(pool, *index.reader);
    auto* supplier = clauses[i].query->createWeight(context, 0)->scorerSupplier(pool, segment);
    if (supplier == nullptr) throw std::runtime_error("empty calibration clause");
    entryStats(i, *supplier);
    if (clauses[i].field == Field::TERM) continue;
    std::string p = "e" + std::to_string(i) + "_";
    auto infos = index.reader->field(clauses[i].name);
    IntColReader reader(segment.postingsReader(), *infos.at(0));
    auto& range = (NumericPredicateQuery&)*clauses[i].query;
    int64_t outside = 0, inside = 0, blocks = 0, runs = 0, lastOutside = -1;
    int64_t outsideBlockRuns = 0;
    int64_t envelope = 0, envelopeEnd = 0;
    for (int64_t block = 0; block < reader.numBlocks(); block++) {
      bool rejected = false;
      int64_t blockRuns = 0;
      range.classifyBlock(reader, block).forEachRun(reader.valuesInBlock(block),
          [&](NumericPredicateQuery::BlockRelation relation, uint32_t begin, uint32_t end) {
            blockRuns++;
            int64_t start = block * IntColReader::BLOCK_SIZE + begin;
            int64_t stop = block * IntColReader::BLOCK_SIZE + end;
            if (relation == NumericPredicateQuery::BlockRelation::OUTSIDE) {
              rejected = true;
              outside += end - begin;
              runs += start != lastOutside;
              lastOutside = stop;
            } else {
              if (relation == NumericPredicateQuery::BlockRelation::INSIDE) inside += end - begin;
              int64_t extended = std::min<int64_t>(size, stop + WINDOW - 1);
              envelope += std::max<int64_t>(0, extended - std::max(start, envelopeEnd));
              envelopeEnd = std::max(envelopeEnd, extended);
            }
          });
      blocks += rejected;
      if (rejected) outsideBlockRuns += blockRuns;
    }
    state.counters[p + "values"] = (double)reader.numValues();
    state.counters[p + "present"] = (double)reader.docsWithValue();
    state.counters[p + "outside"] = (double)outside;
    state.counters[p + "inside"] = (double)inside;
    state.counters[p + "cross"] = (double)(reader.numValues() - outside - inside);
    state.counters[p + "outside_blocks"] = (double)blocks;
    state.counters[p + "outside_runs"] = (double)runs;
    state.counters[p + "outside_block_runs"] = (double)outsideBlockRuns;
    state.counters[p + "envelope"] = (double)envelope;
  }

  auto full = [&](bool prep) {
    MemPool pool;
    Query::Context context(pool, *index.reader);
    auto* supplier = query.createWeight(context, 0)->scorerSupplier(pool, segment);
    auto plan = supplier->planBulk(Use::COUNT_WINDOWS, {});
    if (plan.available != Query::ScorerSupplier::BulkAnswer::YES) {
      throw std::runtime_error("calibration plan declined bulk count");
    }
    auto* bulk = supplier->buildBulk(pool, plan);
    int64_t count = 0;
    if (!prep) {
      for (int32_t cursor = 0; cursor < limit;) {
        cursor = bulk->countNextWindow(count, nullptr, nullptr, cursor, limit);
      }
    }
    benchmark::DoNotOptimize(bulk);
    return count;
  };
  if (mode == "full" || mode == "prep") {
    std::vector<bool> observed(clauses.size());
    std::vector<bool> candidate(clauses.size());
    std::array<std::vector<size_t>, 2> stageOrder;
    control.observe = [&](size_t ordinal, Consumption stage, size_t position,
                          Query::ScorerSupplier& supplier,
                          const Query::PlanContext& context, const Query::ScorerShape& shape) {
      bool isCandidate = stage == Consumption::CANDIDATES;
      bool lead = !isCandidate && position == 0;
      if (ordinal >= clauses.size() || observed[ordinal]
          || (stage != Consumption::WINDOWS && stage != Consumption::CANDIDATES)
          || position != stageOrder[isCandidate].size()
          || shape.membershipConsumption != stage
          || (!order.empty() && lead != (ordinal == order[0]))
          || (!consumption.empty() && stage != consumption[ordinal])) {
        throw std::runtime_error("planner did not honor the forced plan");
      }
      observed[ordinal] = true;
      candidate[ordinal] = isCandidate;
      stageOrder[isCandidate].push_back(ordinal);
      entryStats(ordinal, supplier);
      std::string p = "e" + std::to_string(ordinal) + "_";
      state.counters[p + "lead"] = lead;
      state.counters[p + "candidate"] = isCandidate;
      state.counters[p + "position"] = (double)position;
      state.counters[p + "demand_candidates"] = (double)context.demand.candidates;
      state.counters[p + "demand_span"] = (double)context.demand.span;
    };
    {
      SkipStatsScope stats;
      if (full(false) != expected) throw std::runtime_error("forced plan disagrees with fixture formula");
      statistics(state);
    }
    if ((clauses.size() > 1 || !order.empty() || !consumption.empty())
        && !std::ranges::all_of(observed, [](bool seen) { return seen; })) {
      throw std::runtime_error("query bypassed exhaustive conjunction planning");
    }
    if (!order.empty()) {
      std::array<std::vector<size_t>, 2> expectedOrder;
      for (size_t ordinal : order) expectedOrder[candidate[ordinal]].push_back(ordinal);
      if (stageOrder != expectedOrder) {
        throw std::runtime_error("planner changed the forced per-stage order");
      }
    }
    control.observe = {};
    for (auto _ : state) {
      int64_t count = full(mode == "prep");
      benchmark::DoNotOptimize(count);
    }
    return;
  }

  MemPool pool;
  Query::Context context(pool, *index.reader);
  auto* weight = clauses[0].query->createWeight(context, 0);
  auto* supplier = weight->scorerSupplier(pool, segment);
  auto buildContext = supplier->makePlanContext(Query::Demand::fromCandidatesAndSpan(
      (int64_t)targets.size(), size, Use::COUNT_WINDOWS));
  if (mode == "verify") buildContext.membershipConsumption = Consumption::CANDIDATES;
  auto* plan = supplier->resolve(pool, buildContext);
  Query::Scorer* scorer = nullptr;
  Query::MembershipProof* proof = nullptr;
  {
    SkipStatsScope stats;
    if (mode == "scan" || mode == "zone") {
      auto* numeric = (NumericPredicateQuery::Weight*)weight;
      scorer = mode == "scan" ? numeric->createFullScanScorerForTests(pool, segment)
                               : numeric->createZoneMapScorerForTests(pool, segment);
    } else if (mode == "proof" || mode == "proof_build") {
      proof = plan->buildMembershipProof(pool);
      state.counters["proof_available"] = proof != nullptr;
    } else if (mode != "empty" && mode != "copy" && mode != "points_build"
               && mode != "supplier") {
      scorer = plan->build(pool);
    }
    statistics(state);
  }
  auto kernel = [&] {
    if (mode == "supplier") {
      MemPool scratch;
      auto* s = weight->scorerSupplier(scratch, segment);
      benchmark::DoNotOptimize(s);
      return (int64_t)0;
    }
    if (mode == "points_build") {
      MemPool scratch;
      auto* s = ((NumericPredicateQuery::Weight*)weight)->createPointsScorerForTests(scratch, segment);
      benchmark::DoNotOptimize(s);
      return (int64_t)0;
    }
    if (mode == "proof_build") {
      MemPool scratch;
      auto* p = plan->buildMembershipProof(scratch);
      benchmark::DoNotOptimize(p);
      return (int64_t)0;
    }
    if (scorer != nullptr) { scorer->reset(); scorer->prepareMembership(); }
    if (proof != nullptr) proof->reset();
    int64_t count = 0;
    if (mode == "seek") {
      for (auto doc : targets) count += scorer->advanceWindow(doc, doc + 1) == doc;
      return count;
    }
    std::array<uint64_t, WINDOW / 64> words;
    for (int32_t base = 0; base < size; base += WINDOW) {
      int32_t end = std::min(size, base + WINDOW);
      auto output = std::span(words).first((end - base + 63) / 64);
      if (mode == "verify" || mode == "copy" || mode == "proof") {
        std::copy_n(candidates.begin() + base / 64, output.size(), output.begin());
        if (mode == "verify") scorer->intersectCandidateBits(output, base);
        if (mode == "proof" && proof != nullptr) {
          int32_t possible = proof->nextPossible(base);
          if (possible != base || end > proof->possibleEnd()) proof->intersect(output, base);
        }
      } else {
        std::fill(output.begin(), output.end(), 0);
        if (scorer != nullptr) scorer->fillWindowBits(output, base, end);
      }
      for (auto bits : output) count += std::popcount(bits);
    }
    return count;
  };
  int64_t answer;
  {
    SkipStatsScope stats;
    answer = kernel();
    // Preserve construction arm counters; these counters describe only the walk.
    state.counters["posting_blocks"] = (double)SkipStats::docBlocksDecoded;
    state.counters["fill_docs"] = (double)SkipStats::countBulkFillDocs;
    state.counters["fill_calls"] = (double)SkipStats::countBulkFillCalls;
    state.counters["fill_word_blocks"] = (double)SkipStats::countBulkFillWordBlocks;
    state.counters["advances"] = (double)SkipStats::advanceCalls;
    state.counters["l0_steps"] = (double)SkipStats::l0HeaderSteps;
    state.counters["l1_steps"] = (double)SkipStats::l1GroupSteps;
  }
  if ((mode == "verify" || mode == "seek") && answer != candidateMatches) {
    throw std::runtime_error("candidate kernel disagrees with fixture formula");
  }
  if ((mode == "scan" || mode == "zone" || mode == "fill") && answer != truth[0]) {
    throw std::runtime_error("production kernel disagrees with fixture formula");
  }
  state.counters["kernel_result"] = (double)answer;
  for (auto _ : state) {
    int64_t count = kernel();
    benchmark::DoNotOptimize(count);
    if (count != answer) state.SkipWithError("kernel reset changed result");
  }
}

BENCHMARK_CAPTURE(calibration, kernels, nullptr)
    ->Name("Tuning/ConjunctionCalibration")->UseRealTime();
BENCHMARK_CAPTURE(calibration, r11_cross_ba, "b_i:40000:59999,a_i:49500:50499")
    ->Name("Tuning/ConjunctionHeadroom/r11_cross_ba")->UseRealTime();
BENCHMARK_CAPTURE(calibration, r12_one_column, "t40,b_i:0:89999,a_i:49995:50004")
    ->Name("Tuning/ConjunctionHeadroom/r12_one_column")->UseRealTime();
BENCHMARK_CAPTURE(calibration, r12_many_columns,
    "t40,b_i:0:89999,mixed_i:0:89999,a_i:49995:50004")
    ->Name("Tuning/ConjunctionHeadroom/r12_many_columns")->UseRealTime();

} // namespace
