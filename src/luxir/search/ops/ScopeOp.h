// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "luxir/query/ToParentQuery.h"
#include "luxir/reader/OwnerMap.h"
#include "luxir/search/SearchEngine.h"
#include "luxir/search/ops/SearchOp.h"
#include "luxir/util/ApiError.h"

namespace luxir {

// A request over another nested scope's reader and schema, run serially for
// an owner request (scope changes in the operation tree, per-row operations).
// It runs one operation; its root domain per segment is `base` (live-filtered,
// null = every row) and it assembles one final response, kept alive (and the
// result views into it valid) until the handle is released.
class NestedScopeRequest final : private std::pair<api::SearchRequest, int>, public SearchRequest {
public:
  SearchResponse* final = nullptr;
  std::vector<std::shared_ptr<const void>> held;  // storage of the base domains

  NestedScopeRequest(SearchEngine& engine, const api::SearchRequest& proto, google::protobuf::Arena& arena)
    : std::pair<api::SearchRequest, int>(proto, 0), SearchRequest(engine, this->first, arena) {}

  ReplyStatus reply(SearchResponse& response) override {
    final = &response;
    return ReplyStatus::OK;
  }
  void replyCallback(SearchResponse& response) override { unused(response); }

  struct Release {
    void operator()(NestedScopeRequest* request) const noexcept { if (request) request->done(); }
  };
  using Handle = std::unique_ptr<NestedScopeRequest, Release>;

  static Handle run(SearchRequest& owner, IndexReader& target, std::string_view name, const api::SearchOp& op,
                    std::vector<DocSet*> base, std::vector<std::shared_ptr<const void>> held) {
    auto* arena = createArena();
    auto* proto = (api::SearchRequest*)arena->AllocateAligned(sizeof(api::SearchRequest), alignof(api::SearchRequest));
    new (proto) api::SearchRequest(owner.proto);
    using Pair = std::pair<std::string_view, ::hpp_proto::indirect_view<api::SearchOp>>;
    auto* copy = (api::SearchOp*)arena->AllocateAligned(sizeof(api::SearchOp), alignof(api::SearchOp));
    new (copy) api::SearchOp(op);
    auto* pair = (Pair*)arena->AllocateAligned(sizeof(Pair), alignof(Pair));
    new (pair) Pair{name, copy};
    proto->ops = decltype(proto->ops)(std::span<const Pair>(pair, 1));
    proto->min_commit = {};
    proto->profile = false;
    proto->max_parallel = 0;

    Handle request(luxir::arenaCreate<NestedScopeRequest>(*arena, owner.engine, *proto, *arena));
    auto& scoped = *request;
    scoped.presetResources = true;
    scoped.singleResponse = true;
    scoped.reader = std::shared_ptr<IndexReader>(owner.reader, &target);
    scoped.schema = target.schema();
    scoped.timeZone = owner.timeZone;
    scoped.docFormatDefault = owner.docFormatDefault;
    scoped.baseDomains = std::move(base);
    scoped.held = std::move(held);
    owner.engine.executeScoped(scoped);

    if (scoped.final == nullptr) throw std::logic_error("nested scope request produced no response");
    for (const auto& warning : scoped.warnings) owner.warnOnce(warning.code, warning.message);
    auto& response = scoped.final->proto;
    if (response.error) {
      throw ApiError((ErrorKind)response.error->kind, std::string(response.error->code),
                     std::string(response.error->message));
    }
    return request;
  }

  const api::Val* result(std::string_view name) const {
    if (!final) return nullptr;
    auto* found = final->proto.ops.find(name);
    return found ? &**found : nullptr;
  }
};

// A nested-scope change in the operation tree (Domain.to_children /
// Domain.to_parent). The wrapped operation, with its whole subtree, runs as a
// NestedScopeRequest over the destination scope, so every existing operation
// (lists, facets, metrics, collapse, diversify) runs unchanged there. Its root
// domain is the incoming domain descended to the live elements of its owners
// (to_children) or projected to the distinct owners of its elements
// (to_parent). This operation's result slot references the inner response.
class ScopeOp final : public SearchOp {
  IndexReader& target;
  bool toChildren;
  api::SearchOp inner;
  NestedScopeRequest::Handle request;
  std::mutex runMutex;

public:
  ScopeOp(SearchRequest& req, std::string_view name, IndexReader& target, bool toChildren,
          const api::SearchOp& inner)
    : SearchOp(req, name), target(target), toChildren(toChildren), inner(inner) {}

  // Element rows of `owners` (null = all), or owner rows of `elements`.
  std::shared_ptr<LiveDocs> transform(size_t segment, DocSet* incoming) {
    auto& from = req.reader->segments()[segment];
    auto& to = target.segments()[segment];
    if (toChildren) return descendDomain(from, incoming, to);
    // Project elements to their distinct owners.
    int32_t owners = to.maxDoc();
    std::vector<uint64_t> words(FixedBitSet::sizeInWords(owners), 0);
    int32_t count = 0;
    if (from.maxDoc() > 0) {
      const auto& map = from.postingsReader().owners();
      OwnerMap::OwnerCursor ownerOf(map);
      for (int32_t row = 0; row < from.maxDoc(); row++) {
        if (incoming ? !incoming->get(row) : (from.liveDocSet() && !from.liveDocSet()->get(row))) continue;
        int32_t owner = ownerOf.ownerOf(row);
        uint64_t bit = (uint64_t)1 << (owner & 63);
        if ((words[(size_t)owner >> 6] & bit) == 0) {
          words[(size_t)owner >> 6] |= bit;
          count++;
        }
      }
    }
    return std::make_shared<LiveDocs>(std::move(words), owners, count);
  }

  void run(std::span<const DomainHandle> domains) {
    std::lock_guard lock(runMutex);
    std::vector<std::shared_ptr<const void>> held;
    std::vector<DocSet*> base(target.segments().size(), nullptr);
    for (size_t i = 0; i < base.size(); i++) {
      DocSet* incoming = i < domains.size() ? domains[i].get() : nullptr;
      auto set = transform(i, incoming);
      base[i] = set ? (DocSet*)&set->docset() : (DocSet*)target.segments()[i].liveDocSet();
      held.push_back(std::move(set));
    }
    request = NestedScopeRequest::run(req, target, name, inner, std::move(base), std::move(held));
  }

  const api::Val* result() const { return request ? request->result(name) : nullptr; }

  class Calc final : public Calculator {
    ScopeOp& scope;
    std::vector<DomainHandle> domains;
    std::atomic<size_t> remaining;

    void finish() {
      scope.run(domains);
      if (const auto* value = scope.result()) {
        getTarget(nullptr, [&](api::Val& target) {
          slotTarget<api::ArrVal>(target, op.req.lastResponse->mr) = *value;
        });
      }
    }

  public:
    Calc(ScopeOp& scope, Calculator* parent, int64_t slot, int64_t numSlots,
         const StructuralConstraint* structural)
      : Calculator(scope, parent, slot, numSlots, structural), scope(scope),
        domains(scope.req.reader->segments().size()), remaining(scope.req.reader->segments().size()) {}

    api::Val* getTargetForSub(SearchResponse* resp, SearchOp& sub) override {
      unused(resp, sub);
      return nullptr;
    }

    void calc(oneapi::tbb::task_group* tg, int32_t segnum, DomainHandle domain) override {
      unused(tg);
      if (segnum == -1) {
        finish();
        return;
      }
      domains[(size_t)segnum] = std::move(domain);
      if (remaining.fetch_sub(1) == 1) finish();
    }
  };

  Calculator* createCalculator(Calculator* parent, int64_t slot = -1, int64_t numSlots = -1,
                               const StructuralConstraint* structural = nullptr) override {
    return new Calc(*this, parent, slot, numSlots, structural);
  }
};

} // namespace luxir
