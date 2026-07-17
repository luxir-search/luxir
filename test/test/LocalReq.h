#pragma once

#include "TestUtils.h"
#include "solux/search/SearchRequest.h"

#include <cassert>
#include <deque>
#include <span>
#include <variant>

namespace solux::test {

// SearchResponse.error is a bare string (empty == success); restores the has_error predicate.
inline bool hasError(const RespProto& r) { return !r.error.empty(); }

// Base-from-member: holds the NON-OWNING request view so it exists before the SearchRequest
// base ctor (which borrows it) runs.
struct LocalReqViewHolder {
  ReqProto view;
};

using OpsMap = solux::api::map_view<std::string_view, ::hpp_proto::indirect_view<solux::api::SearchOp>>;

class LocalReq;

// Fluent cursor over one op (or the request root). Cursors are owned by LocalReq in a
// std::deque, so a reference stays valid as later chain calls add more cursors. The configure
// methods assert the op kind. Descend with topDocs()/facet()/avg()/...; climb back with end().
//
// The concrete solux::api classes are the public API: reads go through their named accessors
// (val.docList()/asDouble(), map_view.find). This cursor only hides the ARENA/build-by-backing
// mechanics on the write side. For rare wrapper queries (ConstantScore) use
// rawQuery() and build the wrapper chain on the concrete classes directly.
class OpCursor {
  friend class LocalReq;
  LocalReq* req_;
  OpCursor* parent_;            // null at the root cursor
  solux::api::SearchOp* op_;    // the op this cursor configures; null at root
  OpsMap* subOps_;              // ops map this cursor's children go into; null if op has none
  OpCursor(LocalReq* req, OpCursor* parent, solux::api::SearchOp* op, OpsMap* subOps)
    : req_(req), parent_(parent), op_(op), subOps_(subOps) {}

public:
  // --- descend: add a sub-op into this cursor's ops map, return its cursor ---
  OpCursor& topDocs(std::string_view name);
  OpCursor& facet(std::string_view name, std::string_view field);       // FieldFacet
  OpCursor& rangeFacet(std::string_view name, std::string_view field);  // RangeFacet
  OpCursor& avg(std::string_view name, std::string_view field);         // GenOp "avg"
  OpCursor& min(std::string_view name, std::string_view field);         // GenOp "min"
  OpCursor& max(std::string_view name, std::string_view field);         // GenOp "max"
  OpCursor& stats(std::string_view name, std::string_view field);       // GenOp "stats"

  // --- configure a TopDocs/Fusion op (assert kind) ---
  OpCursor& allQuery();
  OpCursor& existsQuery(std::string_view field);
  OpCursor& matchQuery(std::string_view field, std::string_view value);
  OpCursor& matchQuery(std::string_view field, std::string_view value, solux::api::Match_::Operator op);
  OpCursor& matchFilter(std::string_view name, std::string_view field, std::string_view value);  // append to TopDocs.filter
  OpCursor& prefixQuery(std::string_view field, std::string_view prefix);
  OpCursor& fuzzyQuery(std::string_view field, std::string_view term,
                       int maxEdits = -1, int prefixLength = -1, int maxExpansions = 0);
  OpCursor& simpleQuery(std::string_view q, std::initializer_list<std::string> fieldNames);
  OpCursor& exprQuery(std::string_view q);
  OpCursor& phraseQuery(std::string_view field, std::initializer_list<std::string> words);
  OpCursor& phraseText(std::string_view field, std::string_view text);
  OpCursor& phraseTerms(std::string_view field, std::initializer_list<std::string> terms);
  OpCursor& fields(std::initializer_list<std::string> fieldNames);
  OpCursor& fields(std::span<const std::string> fieldNames);
  OpCursor& batchSize(int32_t n);
  OpCursor& getNumber(bool v = true);
  OpCursor& getScores(bool v = true);
  OpCursor& documentFormat(solux::api::DocFormat v);
  OpCursor& withStats() { return getNumber().getScores(); }

  // --- configure a facet op ---
  OpCursor& limit(int64_t n);     // TopDocs.limit or FieldFacet.limit, by op kind
  OpCursor& mincount(int64_t n);  // FieldFacet / RangeFacet
  OpCursor& range(int64_t start, int64_t end, int64_t gap);  // RangeFacet bounds
  OpCursor& rangeFp(double start, double end, double gap);
  OpCursor& range(std::string_view start, std::string_view end, int64_t gap);
  OpCursor& calendarRange(
      std::string_view start, std::string_view end, int32_t n,
      solux::api::CalendarGap_::Unit unit, std::string_view timeZone = {});

  // --- escape hatch: the mutable arena Query& of this TopDocs op, for wrapper queries
  //     (ConstantScore) the fluent helpers don't cover. ---
  solux::api::Query& rawQuery();

  // --- escape hatches for op fields the fluent helpers don't cover (sorts, knn, fusion
  //     sources, ...): the raw op this cursor configures, and the request build arena.
  //     Build directly on the concrete classes (see QueryBuild.h for arena helpers). ---
  solux::api::SearchOp& rawOp() { return *op_; }
  std::pmr::memory_resource& mr();

  OpCursor& end() { return parent_ ? *parent_ : *this; }

private:
  solux::api::TopDocs& asTopDocs();          // assert + return the TopDocs arm
  solux::api::Query& getOrCreateQuery();      // get-or-create the TopDocs query
  OpCursor& genOpHelper(std::string_view name, std::string_view fn, std::string_view field);
};

// In-process search-request harness. Builds a CONCRETE solux::api::SearchRequest (`view`)
// directly into the request arena via the OpCursor builder (no owning builder, no wire
// bridge). Responses are NON-OWNING views backed by per-response arenas, so reply() RETAINS
// each response (and its arena) until done() - it does not copy or free eagerly. Use the
// RAII handle (localReq()) so done() runs automatically.
class LocalReq : private LocalReqViewHolder, public SearchRequest {
  friend class OpCursor;

public:
  solux::ArenaResource mr;                  // pmr view over the base arena for build helpers
  std::vector<SearchResponse*> responses;   // retained (arenas kept alive until done())

  static LocalReq* create(SearchEngine& engine, google::protobuf::Arena* arena = nullptr) {
    arena = arena ? arena : createArena();
    return solux::arenaCreate<LocalReq>(*arena, engine, *arena);
  }

  LocalReq(SearchEngine& engine, google::protobuf::Arena& arena)
    : SearchRequest(engine, this->view, arena), mr(&arena), rootCursor_(this, nullptr, nullptr, &view.ops) {
  }

  ReplyStatus reply(SearchResponse& response) override {
    responses.push_back(&response);  // retain; the response's arena stays alive until done()
    return ReplyStatus::OK;
  }
  void replyCallback(SearchResponse& response) override { unused(response); }

  // Release each retained response's non-shared arena, then the request arena. Invoked by the
  // RAII handle (or directly); the engine does not auto-call done() for LocalReq.
  void done() override {
    rootCalc.reset();  // calculator tree references op/collector state; drop before the arena
    for (auto* r : responses) {
      if (&r->arena != &this->SearchRequest::arena) releaseArena(&r->arena);
    }
    releaseArena(&this->SearchRequest::arena);
  }

  // --- request-level setters ---
  LocalReq& collection(std::string_view name) {
    if (!view.collection) view.collection.emplace();
    appendStr(view.collection->name, name);
    return *this;
  }
  LocalReq& requestId(std::string_view id) {
    view.request_id = build::arenaStr(mr, id);
    return *this;
  }
  LocalReq& timeZone(std::string_view zone) {
    view.time_zone = build::arenaStr(mr, zone);
    SearchRequest::timeZone = resolveTimeZone(view.time_zone);
    timeZoneError = SearchRequest::timeZone
        ? std::string{} : timeZoneResolutionError(view.time_zone);
    return *this;
  }

  // --- top-level op builders (descend from the root) ---
  OpCursor& topDocs(std::string_view name = "q") { return rootCursor_.topDocs(name); }
  OpCursor& facet(std::string_view name, std::string_view field) { return rootCursor_.facet(name, field); }
  OpCursor& rangeFacet(std::string_view name, std::string_view field) { return rootCursor_.rangeFacet(name, field); }
  OpCursor& avg(std::string_view name, std::string_view field) { return rootCursor_.avg(name, field); }
  OpCursor& min(std::string_view name, std::string_view field) { return rootCursor_.min(name, field); }
  OpCursor& max(std::string_view name, std::string_view field) { return rootCursor_.max(name, field); }
  OpCursor& stats(std::string_view name, std::string_view field) { return rootCursor_.stats(name, field); }

  LocalReq& execute(bool parallel = true) {
    engine.submit(*this, parallel);
    return *this;
  }

  // --- result inspection (reads go through the concrete classes' accessors) ---
  bool ok() const { return !responses.empty() && !hasError(responses[0]->proto); }
  std::string errorMsg() const { return responses.empty() ? "(no response)" : std::string(responses[0]->proto.error); }

  // Declared degradations (SearchResponse.warnings) ride on the final response.
  std::span<const solux::api::Warning> respWarnings() const {
    return responses.empty() ? std::span<const solux::api::Warning>{}
                             : responses.back()->proto.warnings;
  }
  bool hasWarning(std::string_view code) const {
    for (const auto& w : respWarnings()) {
      if (w.code == code) return true;
    }
    return false;
  }

  // The DocList for op `opName` in the first response, or null.
  const solux::api::DocList* docList(std::string_view opName = "q") const {
    const solux::api::Val* v = opVal(opName);
    return v ? v->docList() : nullptr;
  }
  // A scalar (e.g. avg/stats) op result. T is one of int64_t/double/float/bool/string_view.
  template <class T>
  T scalar(std::string_view opName) const {
    const solux::api::Val* v = opVal(opName);
    assert(v != nullptr);
    return std::get<T>(v->kind);
  }
  std::vector<Doc> getDocs(std::string_view opName = "q") const {
    const auto* dl = docList(opName);
    return dl ? convertResultsToDocs(*dl) : std::vector<Doc>{};
  }
  int64_t getMatchCount(std::string_view opName = "q") const {
    const auto* dl = docList(opName);
    return (dl && dl->matches) ? *dl->matches : 0;
  }

  // Compact textual dump for test-failure diagnostics.
  std::string toString() const {
    std::string ret = "Request id=" + std::string(view.request_id) + " ops=[";
    bool first = true;
    for (const auto& [name, op] : view.ops) { ret += (first ? "" : ","); ret += std::string(name); first = false; }
    ret += "]\n";
    for (size_t r = 0; r < responses.size(); r++) {
      const auto& resp = responses[r]->proto;
      ret += "  Response[" + std::to_string(r) + "]";
      if (!resp.error.empty()) ret += " error=\"" + std::string(resp.error) + "\"";
      ret += std::string(" more=") + (resp.more ? "true" : "false") + "\n";
      for (const auto& [name, valPtr] : resp.ops) {
        ret += "    " + std::string(name) + " -> ";
        if (const auto* dl = valPtr->docList()) {
          ret += "DocList(matches=" + (dl->matches ? std::to_string(*dl->matches) : std::string("unset"))
               + ", offset=" + std::to_string(dl->offset) + ", cols=[";
          bool fc = true;
          for (const auto& [cn, col] : dl->columns) { ret += (fc ? "" : ","); ret += std::string(cn); fc = false; }
          ret += "])\n";
        } else {
          ret += "kind#" + std::to_string(valPtr->kind.index()) + "\n";
        }
      }
    }
    return ret;
  }

private:
  static constexpr std::size_t OPS_CAP = 32;  // unused by realloc-grow path; kept for clarity
  std::deque<OpCursor> cursors_;              // stable storage for cursor refs
  OpCursor rootCursor_;                        // root (op-less) cursor; children go into view.ops

  // Arena-allocate (placement-new) a default T. All solux::api types are trivially
  // destructible, so the arena is dropped without running dtors.
  template <class T>
  T* arenaNew() { return new (mr.allocate(sizeof(T), alignof(T))) T(); }

  // Append a SearchOp* into an ops map (realloc-grow: the builder doesn't know the count up
  // front, so grow the backing pair[] by one and copy the prior entries).
  void appendOp(OpsMap& m, std::string_view name, solux::api::SearchOp* sub) {
    using Pair = std::pair<std::string_view, ::hpp_proto::indirect_view<solux::api::SearchOp>>;
    auto old = m;
    Pair* a = build::allocArray(m, old.size() + 1, mr);
    for (std::size_t i = 0; i < old.size(); i++) a[i] = old[i];
    a[old.size()] = Pair{build::arenaStr(mr, name), {sub}};
  }
  OpCursor& pushCursor(OpCursor* parent, solux::api::SearchOp* op, OpsMap* subOps) {
    return cursors_.emplace_back(OpCursor(this, parent, op, subOps));
  }

  // Append one arena-copied string to a span member (realloc-copy preserves prior entries).
  void appendStr(std::span<const std::string_view>& member, std::string_view s) {
    auto old = member;
    std::string_view* a = build::allocArray(member, old.size() + 1, mr);
    for (std::size_t i = 0; i < old.size(); i++) a[i] = old[i];
    a[old.size()] = build::arenaStr(mr, s);
  }
  void setStrs(std::span<const std::string_view>& member, std::initializer_list<std::string> strs) {
    std::string_view* a = build::allocArray(member, strs.size(), mr);
    std::size_t i = 0;
    for (const auto& s : strs) a[i++] = build::arenaStr(mr, s);
  }

  static std::string bytesToStr(::hpp_proto::bytes_view b) {
    return std::string(reinterpret_cast<const char*>(b.data()), b.size());
  }

  const solux::api::Val* opVal(std::string_view opName) const {
    if (responses.empty()) return nullptr;
    const auto* p = responses[0]->proto.ops.find(opName);  // const indirect_view<Val>* or null
    return p ? &**p : nullptr;
  }

  // Convert a DocList result back to Doc rows: document i is the merge of
  // columns row i and docs[i], with per-column missing slots and absent row
  // keys skipped.  This is the reference client decode loop for the flat
  // columns+docs pair.
  static std::vector<Doc> convertResultsToDocs(const solux::api::DocList& docs) {
    std::vector<Doc> results;
    size_t numDocs = (size_t)docs.row_count;

    // Column heights and docs alignment validate against the authoritative count.
    for (const auto& [fieldName, column] : docs.columns) {
      std::visit([&](const auto& col) {
        using C = std::decay_t<decltype(col)>;
        if constexpr (!std::is_same_v<C, std::monostate>) {
          assert(col.v.size() == numDocs && "DocList column height != row_count");
          (void)col;
        }
      }, column.kind);
      (void)fieldName;
    }
    assert(docs.docs.empty() || docs.docs.size() == numDocs);

    if (numDocs == 0) return results;
    results.resize(numDocs);

    for (const auto& [fieldName, column] : docs.columns) {
      if (fieldName == "_score_") continue;
      for (size_t i = 0; i < numDocs; i++) {
        if (const auto* c = std::get_if<solux::api::ColStr>(&column.kind)) {
          if (i < c->v.size() && c->v[i] != c->missing_val) results[i].push_back({std::string(fieldName), std::string(c->v[i])});
        } else if (const auto* c = std::get_if<solux::api::ColInt>(&column.kind)) {
          if (i < c->v.size() && c->v[i] != c->missing_val) results[i].push_back({std::string(fieldName), (int64_t)c->v[i]});
        } else if (const auto* c = std::get_if<solux::api::ColFloat>(&column.kind)) {
          if (i < c->v.size() && c->v[i] != c->missing_val) results[i].push_back({std::string(fieldName), c->v[i]});
        } else if (const auto* c = std::get_if<solux::api::ColDouble>(&column.kind)) {
          if (i < c->v.size() && c->v[i] != c->missing_val) results[i].push_back({std::string(fieldName), c->v[i]});
        } else if (const auto* c = std::get_if<solux::api::ArrArrStr>(&column.kind)) {
          if (i < c->v.size() && !c->v[i].v.empty()) {
            std::vector<std::string> vals;
            for (const auto& s : c->v[i].v) vals.emplace_back(s);
            results[i].push_back({std::string(fieldName), std::move(vals)});
          }
        } else if (const auto* c = std::get_if<solux::api::ArrArrInt>(&column.kind)) {
          if (i < c->v.size() && !c->v[i].v.empty()) {
            std::vector<int64_t> vals(c->v[i].v.begin(), c->v[i].v.end());
            results[i].push_back({std::string(fieldName), std::move(vals)});
          }
        } else if (const auto* c = std::get_if<solux::api::ArrArrFloat>(&column.kind)) {
          if (i < c->v.size() && !c->v[i].v.empty()) {
            std::vector<float> vals(c->v[i].v.begin(), c->v[i].v.end());
            results[i].push_back({std::string(fieldName), std::move(vals)});
          }
        } else if (const auto* c = std::get_if<solux::api::ArrArrDouble>(&column.kind)) {
          if (i < c->v.size() && !c->v[i].v.empty()) {
            std::vector<double> vals(c->v[i].v.begin(), c->v[i].v.end());
            results[i].push_back({std::string(fieldName), std::move(vals)});
          }
        } else if (const auto* c = std::get_if<solux::api::ColVector>(&column.kind)) {
          if (i < c->v.size() && c->v[i].f32) {
            std::vector<float> vals(c->v[i].f32->v.begin(), c->v[i].f32->v.end());
            results[i].push_back({std::string(fieldName), std::move(vals)});
          }
        } else if (const auto* c = std::get_if<solux::api::MultiVector>(&column.kind)) {
          if (i < c->v.size() && !c->v[i].v.empty()) {
            std::vector<std::vector<float>> vals;
            for (const auto& vec : c->v[i].v) {
              if (vec.f32) vals.emplace_back(vec.f32->v.begin(), vec.f32->v.end());
            }
            results[i].push_back({std::string(fieldName), std::move(vals)});
          }
        }
      }
    }

    // Merge the row side: docs[i] holds document i's fields not in columns.
    for (size_t i = 0; i < docs.docs.size(); i++) {
      for (const auto& [name, valPtr] : docs.docs[i].fields) {
        if (name == "_score_") continue;  // parity with the _score_ column skip above
        const auto& val = *valPtr;
        if (const auto* s = std::get_if<std::string_view>(&val.kind)) {
          results[i].push_back({std::string(name), std::string(*s)});
        } else if (const auto* n = std::get_if<int64_t>(&val.kind)) {
          results[i].push_back({std::string(name), *n});
        } else if (const auto* f = std::get_if<float>(&val.kind)) {
          results[i].push_back({std::string(name), *f});
        } else if (const auto* d = std::get_if<double>(&val.kind)) {
          results[i].push_back({std::string(name), *d});
        } else if (const auto* b = std::get_if<bool>(&val.kind)) {
          results[i].push_back({std::string(name), *b});
        } else if (const auto* a = std::get_if<solux::api::ArrStr>(&val.kind)) {
          std::vector<std::string> vals(a->v.begin(), a->v.end());
          results[i].push_back({std::string(name), std::move(vals)});
        } else if (const auto* a = std::get_if<solux::api::ArrInt>(&val.kind)) {
          std::vector<int64_t> vals(a->v.begin(), a->v.end());
          results[i].push_back({std::string(name), std::move(vals)});
        } else if (const auto* a = std::get_if<solux::api::ArrFloat>(&val.kind)) {
          std::vector<float> vals(a->v.begin(), a->v.end());
          results[i].push_back({std::string(name), std::move(vals)});
        } else if (const auto* a = std::get_if<solux::api::ArrDouble>(&val.kind)) {
          std::vector<double> vals(a->v.begin(), a->v.end());
          results[i].push_back({std::string(name), std::move(vals)});
        } else if (const auto* v = std::get_if<solux::api::Vector>(&val.kind)) {
          if (v->f32) {
            std::vector<float> vals(v->f32->v.begin(), v->f32->v.end());
            results[i].push_back({std::string(name), std::move(vals)});
          }
        } else if (const auto* av = std::get_if<solux::api::ArrVector>(&val.kind)) {
          std::vector<std::vector<float>> vals;
          for (const auto& vec : av->v) {
            if (vec.f32) vals.emplace_back(vec.f32->v.begin(), vec.f32->v.end());
          }
          results[i].push_back({std::string(name), std::move(vals)});
        }
      }
    }
    return results;
  }
};

// ---- OpCursor method definitions (LocalReq is now complete) ----

inline solux::api::TopDocs& OpCursor::asTopDocs() {
  assert(op_ != nullptr && std::holds_alternative<solux::api::TopDocs>(op_->kind));
  return std::get<solux::api::TopDocs>(op_->kind);
}
inline solux::api::Query& OpCursor::getOrCreateQuery() {
  auto& td = asTopDocs();
  if (!td.query.has_value()) td.query = req_->arenaNew<solux::api::Query>();
  return *const_cast<solux::api::Query*>(&*td.query);
}
inline solux::api::Query& OpCursor::rawQuery() { return getOrCreateQuery(); }
inline std::pmr::memory_resource& OpCursor::mr() { return req_->mr; }

inline OpCursor& OpCursor::topDocs(std::string_view name) {
  auto* sub = req_->arenaNew<solux::api::SearchOp>();
  req_->appendOp(*subOps_, name, sub);
  auto& td = sub->kind.emplace<solux::api::TopDocs>();
  return req_->pushCursor(this, sub, &td.ops);
}
inline OpCursor& OpCursor::facet(std::string_view name, std::string_view field) {
  auto* sub = req_->arenaNew<solux::api::SearchOp>();
  req_->appendOp(*subOps_, name, sub);
  auto& f = sub->kind.emplace<solux::api::FieldFacet>();
  f.field = build::arenaStr(req_->mr, field);
  return req_->pushCursor(this, sub, &f.ops);
}
inline OpCursor& OpCursor::rangeFacet(std::string_view name, std::string_view field) {
  auto* sub = req_->arenaNew<solux::api::SearchOp>();
  req_->appendOp(*subOps_, name, sub);
  auto& f = sub->kind.emplace<solux::api::RangeFacet>();
  f.field = build::arenaStr(req_->mr, field);
  return req_->pushCursor(this, sub, &f.ops);
}
inline OpCursor& OpCursor::avg(std::string_view name, std::string_view field) {
  return genOpHelper(name, "avg", field);
}
inline OpCursor& OpCursor::min(std::string_view name, std::string_view field) {
  return genOpHelper(name, "min", field);
}
inline OpCursor& OpCursor::max(std::string_view name, std::string_view field) {
  return genOpHelper(name, "max", field);
}
inline OpCursor& OpCursor::stats(std::string_view name, std::string_view field) {
  return genOpHelper(name, "stats", field);
}
inline OpCursor& OpCursor::genOpHelper(std::string_view name, std::string_view fn, std::string_view field) {
  auto* sub = req_->arenaNew<solux::api::SearchOp>();
  req_->appendOp(*subOps_, name, sub);
  auto& g = sub->kind.emplace<solux::api::GenOp>();
  g.name = build::arenaStr(req_->mr, fn);
  solux::api::Val* args = build::allocArray(g.args, 1, req_->mr);
  args[0].kind = build::arenaStr(req_->mr, field);
  return req_->pushCursor(this, sub, nullptr);  // GenOp has no sub-ops
}

inline OpCursor& OpCursor::allQuery() { getOrCreateQuery().kind = true; return *this; }  // the `all` arm
inline OpCursor& OpCursor::existsQuery(std::string_view field) {
  auto& e = getOrCreateQuery().kind.emplace<solux::api::ExistsQuery>();
  e.field = build::arenaStr(req_->mr, field);
  return *this;
}
inline OpCursor& OpCursor::matchQuery(std::string_view field, std::string_view value) {
  auto& q = getOrCreateQuery();
  auto& m = std::holds_alternative<solux::api::Match>(q.kind)
            ? std::get<solux::api::Match>(q.kind)
            : q.kind.emplace<solux::api::Match>();
  m.field = build::arenaStr(req_->mr, field);
  auto* v = req_->arenaNew<solux::api::Val>();
  v->kind = build::arenaStr(req_->mr, value);
  m.val = v;
  return *this;
}
inline OpCursor& OpCursor::matchQuery(std::string_view field, std::string_view value, solux::api::Match_::Operator op) {
  matchQuery(field, value);
  std::get<solux::api::Match>(getOrCreateQuery().kind).operator_ = op;
  return *this;
}
inline OpCursor& OpCursor::matchFilter(std::string_view name, std::string_view field, std::string_view value) {
  auto& td = asTopDocs();
  auto old = td.filter;
  auto* a = build::allocArray(td.filter, old.size() + 1, req_->mr);
  std::copy(old.begin(), old.end(), a);
  auto& named = a[old.size()];
  named.name = build::arenaStr(req_->mr, name);
  auto* q = req_->arenaNew<solux::api::Query>();
  auto& m = q->kind.emplace<solux::api::Match>();
  m.field = build::arenaStr(req_->mr, field);
  auto* v = req_->arenaNew<solux::api::Val>();
  v->kind = build::arenaStr(req_->mr, value);
  m.val = v;
  named.query = q;
  return *this;
}
inline OpCursor& OpCursor::simpleQuery(std::string_view q, std::initializer_list<std::string> fieldNames) {
  auto& s = getOrCreateQuery().kind.emplace<solux::api::SimpleQuery>();
  s.q = build::arenaStr(req_->mr, q);
  auto* arr = build::allocArray(s.fields, fieldNames.size(), req_->mr);
  size_t i = 0;
  for (const auto& f : fieldNames) {
    arr[i++] = build::arenaStr(req_->mr, f);
  }
  return *this;
}
inline OpCursor& OpCursor::exprQuery(std::string_view q) {
  auto& e = getOrCreateQuery().kind.emplace<solux::api::ExprQuery>();
  e.q = build::arenaStr(req_->mr, q);
  return *this;
}
inline OpCursor& OpCursor::prefixQuery(std::string_view field, std::string_view prefix) {
  auto& p = getOrCreateQuery().kind.emplace<solux::api::PrefixQuery>();
  p.field = build::arenaStr(req_->mr, field);
  p.prefix = build::arenaStr(req_->mr, prefix);
  return *this;
}
inline OpCursor& OpCursor::fuzzyQuery(std::string_view field, std::string_view term,
                                      int maxEdits, int prefixLength, int maxExpansions) {
  auto& f = getOrCreateQuery().kind.emplace<solux::api::FuzzyQuery>();
  f.field = build::arenaStr(req_->mr, field);
  f.term = build::arenaStr(req_->mr, term);
  if (maxEdits >= 0) f.max_edits = maxEdits;
  if (prefixLength >= 0) f.prefix_length = prefixLength;
  if (maxExpansions > 0) f.max_expansions = maxExpansions;
  return *this;
}
inline OpCursor& OpCursor::phraseQuery(std::string_view field, std::initializer_list<std::string> words) {
  auto& p = getOrCreateQuery().kind.emplace<solux::api::PhraseQuery>();
  p.field = build::arenaStr(req_->mr, field);
  req_->setStrs(p.words, words);
  return *this;
}
inline OpCursor& OpCursor::phraseText(std::string_view field, std::string_view text) {
  auto& p = getOrCreateQuery().kind.emplace<solux::api::PhraseQuery>();
  p.field = build::arenaStr(req_->mr, field);
  p.text = build::arenaStr(req_->mr, text);
  return *this;
}
inline OpCursor& OpCursor::phraseTerms(std::string_view field, std::initializer_list<std::string> terms) {
  auto& p = getOrCreateQuery().kind.emplace<solux::api::PhraseQuery>();
  p.field = build::arenaStr(req_->mr, field);
  req_->setStrs(p.terms, terms);
  return *this;
}
inline OpCursor& OpCursor::fields(std::initializer_list<std::string> fieldNames) {
  auto& td = asTopDocs();
  for (const auto& f : fieldNames) req_->appendStr(td.fields, f);
  return *this;
}
inline OpCursor& OpCursor::fields(std::span<const std::string> fieldNames) {
  auto& td = asTopDocs();
  for (const auto& f : fieldNames) req_->appendStr(td.fields, f);
  return *this;
}
inline OpCursor& OpCursor::batchSize(int32_t n) { asTopDocs().batch_size = n; return *this; }
inline OpCursor& OpCursor::getNumber(bool v) { asTopDocs().get_number = v; return *this; }
inline OpCursor& OpCursor::getScores(bool v) { asTopDocs().get_scores = v; return *this; }
inline OpCursor& OpCursor::documentFormat(solux::api::DocFormat v) { asTopDocs().document_format = v; return *this; }

inline OpCursor& OpCursor::limit(int64_t n) {
  if (auto* td = std::get_if<solux::api::TopDocs>(&op_->kind)) td->limit = n;
  else if (auto* f = std::get_if<solux::api::FieldFacet>(&op_->kind)) f->limit = n;
  else assert(false && "limit() on an op without a limit field");
  return *this;
}
inline OpCursor& OpCursor::mincount(int64_t n) {
  if (auto* f = std::get_if<solux::api::FieldFacet>(&op_->kind)) f->mincount = n;
  else if (auto* r = std::get_if<solux::api::RangeFacet>(&op_->kind)) r->mincount = n;
  else assert(false && "mincount() on a non-facet op");
  return *this;
}
inline OpCursor& OpCursor::range(int64_t start, int64_t end, int64_t gap) {
  auto& r = std::get<solux::api::RangeFacet>(op_->kind);
  auto* startVal = req_->arenaNew<solux::api::Val>();
  auto* endVal = req_->arenaNew<solux::api::Val>();
  startVal->kind = start;
  endVal->kind = end;
  r.start = startVal;
  r.end = endVal;
  r.gap_kind.emplace<solux::api::Val>().kind = gap;
  return *this;
}
inline OpCursor& OpCursor::rangeFp(double start, double end, double gap) {
  auto& r = std::get<solux::api::RangeFacet>(op_->kind);
  auto* startVal = req_->arenaNew<solux::api::Val>();
  auto* endVal = req_->arenaNew<solux::api::Val>();
  startVal->kind = start;
  endVal->kind = end;
  r.start = startVal;
  r.end = endVal;
  r.gap_kind.emplace<solux::api::Val>().kind = gap;
  return *this;
}
inline OpCursor& OpCursor::range(
    std::string_view start, std::string_view end, int64_t gap) {
  auto& r = std::get<solux::api::RangeFacet>(op_->kind);
  auto* startVal = req_->arenaNew<solux::api::Val>();
  auto* endVal = req_->arenaNew<solux::api::Val>();
  startVal->kind = build::arenaStr(req_->mr, start);
  endVal->kind = build::arenaStr(req_->mr, end);
  r.start = startVal;
  r.end = endVal;
  r.gap_kind.emplace<solux::api::Val>().kind = gap;
  return *this;
}
inline OpCursor& OpCursor::calendarRange(
    std::string_view start, std::string_view end, int32_t n,
    solux::api::CalendarGap_::Unit unit, std::string_view timeZone) {
  auto& r = std::get<solux::api::RangeFacet>(op_->kind);
  auto* startVal = req_->arenaNew<solux::api::Val>();
  auto* endVal = req_->arenaNew<solux::api::Val>();
  startVal->kind = build::arenaStr(req_->mr, start);
  endVal->kind = build::arenaStr(req_->mr, end);
  r.start = startVal;
  r.end = endVal;
  auto& gap = r.gap_kind.emplace<solux::api::CalendarGap>();
  gap.n = n;
  gap.unit = unit;
  r.time_zone = build::arenaStr(req_->mr, timeZone);
  return *this;
}

// RAII owner: destruction calls done(), releasing the request arena and every retained
// response arena. Use: `auto req = localReq(engine); req->topDocs(...)...; req->execute();`
class LocalReqHandle {
  LocalReq* p_;
public:
  explicit LocalReqHandle(LocalReq* p) : p_(p) {}
  LocalReqHandle(LocalReqHandle&& o) noexcept : p_(o.p_) { o.p_ = nullptr; }
  LocalReqHandle(const LocalReqHandle&) = delete;
  LocalReqHandle& operator=(const LocalReqHandle&) = delete;
  ~LocalReqHandle() { if (p_) p_->done(); }
  LocalReq* operator->() const { return p_; }
  LocalReq& operator*() const { return *p_; }
  LocalReq* get() const { return p_; }
};

inline LocalReqHandle localReq(SearchEngine& engine) { return LocalReqHandle(LocalReq::create(engine)); }

} // namespace solux::test

// Assert/expect the request succeeded (first response present + no error), dumping the
// request/response on failure. Works for a LocalReqHandle or a LocalReq*.
#define ASSERT_OK(req) ASSERT_TRUE((req)->ok()) << (req)->toString()
#define EXPECT_OK(req) EXPECT_TRUE((req)->ok()) << (req)->toString()
