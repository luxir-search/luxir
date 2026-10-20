// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "TopDocsReq.h"
#include "CollapsedPage.h"

namespace luxir {
namespace {
// A ranked source supplies member rows and exact keys/totals. Selection is
// complete; enrichment and streaming preserve the same page/group protocol.
template <typename RankedSource>
void emitPage(const CollapsedPageSettings& settings,
              SearchOp::Calculator& calc, RankedSource& collector) {
  auto& qr = settings.owner;
  auto getDocList = [&calc](SearchResponse* resp) -> api::DocList& {
    return *calc.slotArm<api::DocList>(resp);
  };
  DocEmission emission = calc.underBucketSlot() ? DocEmission::FINAL_RESPONSE : DocEmission::STREAM;
  auto scoreDocs = collector.sort();
  constexpr bool metricOrder = requires { collector.key((size_t)0); };
  auto keyAt = [&](size_t slot) -> const CollapseKey& {
    if constexpr (metricOrder) return collector.key(slot);
    else return collector.groups->keys[slot];
  };
  int64_t groupCount;
  if constexpr (metricOrder) groupCount = collector.totalGroups();
  else groupCount = collector.groups->totalGroups();
  struct Page {
    RequestMemTracker& tracker;
    size_t tails = 0;
    RequestMemResource resource;
    std::pmr::vector<CollapseKey> keys;
    std::pmr::vector<segdoc> heads;
    std::pmr::vector<float> scores;
    std::pmr::vector<int64_t> counts;
    explicit Page(RequestMemTracker& tracker)
        : tracker(tracker), resource(tracker, "page heads", "collapse member state"),
          keys(&resource), heads(&resource), scores(&resource), counts(&resource) {}
    ~Page() { if (tails) tracker.release(tails); }
    void appendKey(const CollapseKey& key) {
      // Inline tuples allocate nothing. Reserve the out-of-line copy's
      // growth allowance before copying wider tuples, without changing
      // the identity representation used by ordinary collection.
      if (key.components.size() > 2) {
        if (key.components.size() > std::numeric_limits<size_t>::max()
            / (2 * sizeof(CollapseKey::Component))) {
          tracker.chargeOverflow("collapse member state", "page tuple components");
        }
        size_t bytes = 2 * key.components.size() * sizeof(CollapseKey::Component);
        tracker.charge(bytes, "collapse member state", "page tuple components");
        tails += bytes;
      }
      keys.push_back(key);
    }
  };
  std::shared_ptr<Page> page;
  int64_t offset = std::min<int64_t>(settings.offset, (int64_t)scoreDocs.size());
  if (settings.members && offset < (int64_t)scoreDocs.size()) {
    page = std::make_shared<Page>(qr.req.memoryTracker);
    size_t size = scoreDocs.size() - (size_t)offset;
    page->keys.reserve(size);
    page->heads.reserve(size);
    page->scores.reserve(size);
    page->counts.resize(size);
    for (size_t i = (size_t)offset; i < scoreDocs.size(); i++) {
      const auto& rank = scoreDocs[i];
      page->appendKey(keyAt((size_t)rank.slot));
      page->heads.push_back(rank.doc);
      page->scores.push_back(rank.score);
    }
    std::unique_ptr<FieldSortCollector> headCollector;
    std::pmr::vector<std::optional<FieldSortCollector::SortDoc>> heads(&page->resource);
    if (settings.headSort) {
      auto clauses = settings.headSort->clauses;
      if (clauses.empty()) clauses.emplace_back(SortClause::SCORE, SortField::DESC);
      headCollector = std::make_unique<FieldSortCollector>(
          (int64_t)size, clauses, qr.req.reader.get(),
          settings.getScores || settings.headSort->rankNeedsScores);
      heads.resize(size);
    }
    // Expand singletons are already complete, including their score.
    // They require neither a key lookup nor a member/scorer pass.
    bool ordinary = false;
    for (size_t slot = 0; slot < size; slot++) {
      if (page->keys[slot].singleton) page->counts[slot] = 1;
      else ordinary = true;
    }
    auto produce = [&]<bool Identity>(size_t segnum, std::span<const CollapseKey> keys,
                       size_t begin, DocSet* eligibility, auto&& accept) {
      auto& segment = qr.req.reader->segments()[segnum];
      auto release = scope_guard([&]() noexcept {
        if constexpr (!Identity) {
          if (settings.releaseSegment) settings.releaseSegment(qr, segment);
        }
      });
      if constexpr (Identity) {
        CollapseMembers members(settings.collapse, keys, qr.req.memoryTracker);
        members.forKeys(segment.ord, eligibility,
            [&](size_t slot, int32_t doc, float) { accept(slot, doc); });
      } else {
        MemPool pool;
        std::optional<FieldSortCollector::ExpressionBindings> expressions;
        if (headCollector) {
          headCollector->setSegment(segment, &pool);
          expressions.emplace(*headCollector, pool, segment);
        }
        Query::SegmentSource& source = *settings.members;
        CollapseMembers members(settings.collapse, keys, qr.req.memoryTracker);
        members.forSegment(segment.ord, eligibility, source,
            headCollector && (settings.getScores || settings.headSort->rankNeedsScores),
            [&](size_t slot, int32_t doc, float score) {
              size_t pageSlot = begin + slot;
              page->counts[pageSlot]++;
              if (headCollector) headCollector->fold(
                  (int32_t)pageSlot, heads[pageSlot], segdoc(segment.ord, doc), score);
              accept(slot, doc);
            });
      }
    };
    if (settings.groupOps.empty()) {
      if (ordinary) for (size_t segnum = 0; segnum < qr.req.reader->segments().size(); segnum++) {
        produce.template operator()<false>(segnum, page->keys, 0,
            settings.domains[segnum].get(), [](size_t, int32_t) {});
      }
    } else {
      settings.pageGroups = build::allocMessage<luxir::api::GroupTable>(qr.req.lastResponse->mr);
      std::vector<SearchOp*> children;
      for (auto& [name, child] : settings.groupOps) children.push_back(child);
      std::vector<SelectedFacetBucket<size_t>> buckets;
      for (size_t slot = 0; slot < size; slot++) buckets.push_back({
          .key = slot, .id = {}, .count = 0,
          .owner = FacetOwnerSlot{(int32_t)slot},
          .output = FacetOutputSlot{(int32_t)slot}});
      // Both strategies share one exact replay scorer per segment/chunk.
      // Treat them as JOINT: a domain-driven chunk must not be rescanned
      // once per child/bucket. The executor bounds binding and domain state.
      auto domains = [&]<bool Identity>(size_t segnum, auto block,
                         const DomainHandle& eligibility, FacetBucketFeed feed) {
        int32_t maxDoc = qr.req.reader->segments()[segnum].maxDoc();
        feedFacetBucketDomainChunks(maxDoc, block,
            FACET_BUCKET_DOMAIN_BYTES, feed, [&](auto chunk) {
              std::pmr::vector<std::pmr::vector<int32_t>> docs(&page->resource);
              docs.resize(chunk.size());
              size_t begin = chunk.front().key;
              auto keys = std::span<const CollapseKey>(page->keys).subspan(begin, chunk.size());
              bool hasOrdinary = false;
              for (size_t slot = 0; slot < keys.size(); slot++) {
                if (!keys[slot].singleton) hasOrdinary = true;
                else if (page->heads[begin + slot].segment() == (int32_t)segnum) {
                  int32_t doc = page->heads[begin + slot].docId();
                  if constexpr (Identity) {
                    if (eligibility.get() && !eligibility.get()->get(doc)) continue;
                  }
                  docs[slot].push_back(doc);
                }
              }
              if (hasOrdinary) produce.template operator()<Identity>(
                  segnum, keys, begin, eligibility.get(),
                  [&](size_t slot, int32_t doc) { docs[slot].push_back(doc); });
              std::pmr::vector<DomainHandle> result(&page->resource);
              result.reserve(chunk.size());
              for (auto& members : docs) {
                if constexpr (Identity) {
                  DocSetBuilder builder(maxDoc);
                  for (int32_t doc : members) builder.add(doc);
                  result.emplace_back(builder.build());
                } else {
                  auto charge = std::make_shared<DomainVariantPlan::DomainCharge>(
                      qr.req.memoryTracker, sizeof(ArrDocSet) + members.size() * sizeof(int32_t),
                      "group bucket domain", "collapse member state");
                  result.push_back(DomainHandle(std::make_unique<ArrDocSet>(
                      std::vector<int32_t>(members.begin(), members.end())))
                      .chargedWith(std::move(charge)));
                }
              }
              return result;
            });
      };
      FacetBucketBlockExecutor::execute<size_t>(calc, children, buckets,
          *qr.req.reader, FacetBucketProducer::JOINT,
          [&](size_t segnum, auto block, FacetBucketFeed feed) {
            domains.template operator()<false>(segnum, block,
                settings.domains[segnum], feed);
          }, FacetBucketBlockExecutor::BINDING_BYTES, settings.domains,
          [&](size_t segnum, auto block, DomainHandle eligibility, FacetBucketFeed feed) {
            domains.template operator()<true>(segnum, block, std::move(eligibility), feed);
          });
    }
    if (headCollector) {
      for (size_t slot = 0; slot < size; slot++) {
        if (!heads[slot]) continue; // singleton, already filled
        page->heads[slot] = heads[slot]->doc;
        page->scores[slot] = heads[slot]->score;
      }
    }
  }
  if (settings.releaseMembers) settings.releaseMembers(calc);
  auto metadata = [&collector, scoreDocs, page, offset, &qr, settings,
                   groupResults = settings.pageGroups, keyAt](
      luxir::api::DocList& list, int64_t begin, int64_t end, std::pmr::memory_resource& mr) {
    if (settings.collapse.getNumber) list.found_docs = collector.totalHits();
    auto* groupTable = build::allocMessage<luxir::api::GroupTable>(mr);
    list.groups = groupTable;
    auto& table = *groupTable;
    if (groupResults) {
      for (auto& [name, column] : groupResults->ops) {
        auto* target = build::opsSlot(table.ops, groupResults->ops.size(), name, mr);
        target->kind = column->kind;
        std::visit([&](auto& array) {
          using T = std::decay_t<decltype(array)>;
          if constexpr (std::is_same_v<T, api::ArrVal> || std::is_same_v<T, api::ArrInt>
              || std::is_same_v<T, api::ArrFloat> || std::is_same_v<T, api::ArrDouble>
              || std::is_same_v<T, api::ArrStr> || std::is_same_v<T, api::ArrBin>
              || std::is_same_v<T, api::ArrVector>) {
            array.v = array.v.subspan((size_t)(begin - offset), (size_t)(end - begin));
          } else { assert(false); }
        }, target->kind);
      }
    }
    auto* keys = build::allocArray(table.keys, (size_t)(end - begin), mr);
    int64_t* counts = page && settings.spec.get_number
        ? build::allocArray(table.counts, (size_t)(end - begin), mr) : nullptr;
    luxir::api::ArrVal* ranks = settings.getScores
        && (metricOrder || settings.alwaysRank || (page && settings.headSort))
        ? build::allocArray(table.ranks, (size_t)(end - begin), mr) : nullptr;
    MemPool pool;
    for (int64_t i = begin; i < end; i++) {
      const auto& rank = scoreDocs[(size_t)i];
      settings.collapse.emit(keys[i - begin],
          keyAt((size_t)rank.slot), mr, pool);
      size_t slot = (size_t)(i - offset);
      if (counts) counts[i - begin] = page->counts[slot];
      if constexpr (metricOrder) {
        if (ranks) collector.emitRank(ranks[i - begin], rank.slot, mr);
      } else if (ranks && (settings.alwaysRank || page->heads[slot] != rank.doc)) {
        emitCollapseRank(ranks[i - begin], settings.sort,
            *qr.req.reader, rank.doc, rank.score, mr);
      }
    }
  };
  emitDocsResponse(qr.req, getDocList,
    (int64_t)scoreDocs.size(),
    [scoreDocs, page, offset](int64_t i) {
      return page ? page->heads[(size_t)(i - offset)] : scoreDocs[(size_t)i].doc;
    },
    [scoreDocs, page, offset](int64_t i) {
      return page ? page->scores[(size_t)(i - offset)] : scoreDocs[(size_t)i].score;
    },
    groupCount, settings.fields,
    settings.batchSize, settings.offset,
    settings.getNumber, settings.getScores,
    settings.documentFormat, emission, std::move(metadata), settings.namedQueries);
}
CollapsedPageSettings pageSettings(TopDocsReq& op, TopDocsReq::CalcBase& calc) {
  return {op, *op.collapse, *op.topDocsProto.collapse, op.sortPlan,
      op.headSort, op.groupOps, op.topDocsProto.fields, op.topDocsProto.batch_size,
      op.topDocsProto.offset, op.topDocsProto.get_number, op.topDocsProto.get_scores,
      op.topDocsProto.document_format,
      calc.memberReplay ? (calc.preparedWeight
          ? static_cast<Query::SegmentSource*>(calc.preparedWeight.get()) : op.weight) : nullptr,
      calc.memberReplay ? std::span<const DomainHandle>(calc.memberReplay->domains)
                        : std::span<const DomainHandle>{}, calc.pageGroups, false,
      [](SearchOp& owner, IndexReader::Segment& segment) { owner.releaseSegmentState(segment); },
      [](SearchOp::Calculator& base) { static_cast<TopDocsReq::CalcBase&>(base).memberReplay.reset(); },
      op.namedQueries};
}
}  // namespace

void emitCollapsedPage(const CollapsedPageSettings& settings,
                       SearchOp::Calculator& calc, ScoreCollector<true>& collector) {
  emitPage(settings, calc, collector);
}

void TopDocsReq::fillQueryTopNResponse(TopDocsReq::CalcBase& calc) {
    auto& qr = *this;
    auto* mergeableCollector = calc.collectorMerger.getData();

    if (mergeableCollector == nullptr) {
      auto& docListProto = *calc.slotArm<luxir::api::DocList>(nullptr);
      docListProto.offset = qr.topDocsProto.offset;
      // found is opt-in (see emitDocsResponse): only populate it when the
      // count was requested, so an empty index matches the non-empty contract.
      if (qr.topDocsProto.get_number) {
        docListProto.found = 0;
        if (qr.collapse != nullptr) docListProto.found_docs = 0;
        if (qr.diversify != nullptr) docListProto.found_groups = 0;
      }
      return;
    }

    // Resolve the DocList arm under the response lock: sub-op calculators
    // set the same arm through getTargetForSub while this emitter runs.
    auto getDocList = [&calc](SearchResponse* resp) -> luxir::api::DocList& {
      return *calc.slotArm<luxir::api::DocList>(resp);
    };
    DocEmission emission = calc.underBucketSlot()
        ? DocEmission::FINAL_RESPONSE : DocEmission::STREAM;

    if (qr.diversify != nullptr) {
      fillDiversifyResponse(calc, *mergeableCollector);
    } else if (qr.collapse != nullptr) {
      if (mergeableCollector->useFieldSort) {
        emitPage(pageSettings(qr, calc), calc, *mergeableCollector->groupFieldCollector);
      }
      else emitPage(pageSettings(qr, calc), calc, *mergeableCollector->groupScoreCollector);
    } else if (mergeableCollector->useFieldSort) {
      auto& collector = *mergeableCollector->fieldCollector;
      auto sortDocs = collector.sort();
      emitDocsResponse(qr.req, getDocList,
        (int64_t)sortDocs.size(),
        [sortDocs](int64_t i) { return sortDocs[i].doc; },
        [sortDocs](int64_t i) { return sortDocs[i].score; },
        collector.totalHits(),
        qr.topDocsProto.fields,
        qr.topDocsProto.batch_size,
        qr.topDocsProto.offset,
        qr.topDocsProto.get_number,
        qr.topDocsProto.get_scores,
        qr.topDocsProto.document_format,
        emission, {}, qr.namedQueries);
    } else {
      auto& collector = *mergeableCollector->scoreCollector;
      auto scoreDocs = collector.sort();
      emitDocsResponse(qr.req, getDocList,
        (int64_t)scoreDocs.size(),
        [scoreDocs](int64_t i) { return scoreDocs[i].doc; },
        [scoreDocs](int64_t i) { return scoreDocs[i].score; },
        collector.totalHits(),
        qr.topDocsProto.fields,
        qr.topDocsProto.batch_size,
        qr.topDocsProto.offset,
        qr.topDocsProto.get_number,
        qr.topDocsProto.get_scores,
        qr.topDocsProto.document_format,
        emission, {}, qr.namedQueries);
    }
  }

void TopDocsReq::fillMetricGroupResponse(CalcBase& calc) {
  auto* data = calc.metricGroups->merger.getData();
  if (data) emitPage(pageSettings(*this, calc), calc, *data);
  else {
    auto& list = *calc.slotArm<api::DocList>(nullptr);
    list.offset = topDocsProto.offset;
    if (topDocsProto.get_number) { list.found = 0; list.found_docs = 0; }
  }
}
}  // namespace luxir
