#pragma once

#include "TestUtils.h"
#include "solux/index/IndexWriter.h"
#include "solux/schema/Schema.h"
#include "solux/server/ProtoUpdateMessage.h"
#include "solux/search/SearchEngine.h"
#include "solux/api/build.h"
#include "LocalReq.h"
#include <memory_resource>
#include <typeinfo>

#include "solux/util/thread.h"

namespace solux::test {

namespace build = solux::api::build;

// Owning result of a blocking update. The wire response is NON-OWNING (backed by the
// message's arena, which dies with the message), so the blocking done() EXTRACTS owning
// copies of everything a test might read after the call returns.
struct IndexResult {
  using Status = solux::api::UpdateResponse_::Status;
  struct Error {
    std::string id;
    std::string error_message;
    int32_t index = 0;
  };

  uint64_t updateVersion = 0;
  bool success = false;  // false if the message errored or the response status is ERROR
  Status status = Status::UNKNOWN;
  std::string error_message;          // message-level error, if any
  std::vector<std::string> ids;       // returned ids (return_ids requests)
  std::vector<Error> errors;          // per-doc errors
};

class CollectionHelper {
private:
  std::shared_ptr<Collection> collection_;

  static bool fieldTypesEqual(const FieldType& lhs, const FieldType& rhs) {
    if (lhs.type_ != rhs.type_
        || lhs.name_ != rhs.name_
        || lhs.flags_ != rhs.flags_
        || lhs.storedResource_ != rhs.storedResource_) {
      return false;
    }
    if (auto* l = dynamic_cast<const TextFieldType*>(&lhs)) {
      auto* r = dynamic_cast<const TextFieldType*>(&rhs);
      return r != nullptr && l->tokenizer_ == r->tokenizer_ && l->filters_ == r->filters_;
    }
    if (auto* l = dynamic_cast<const VectorFieldType*>(&lhs)) {
      auto* r = dynamic_cast<const VectorFieldType*>(&rhs);
      return r != nullptr
             && l->dims_ == r->dims_
             && l->metric_ == r->metric_
             && l->normalized_ == r->normalized_
             && l->normalizeOnWrite_ == r->normalizeOnWrite_;
    }
    if (auto* l = dynamic_cast<const StoredFieldType*>(&lhs)) {
      auto* r = dynamic_cast<const StoredFieldType*>(&rhs);
      return r != nullptr
             && l->codec_ == r->codec_
             && l->chunkTargetUncompressed_ == r->chunkTargetUncompressed_
             && l->maxDocsPerChunk_ == r->maxDocsPerChunk_;
    }
    return typeid(lhs) == typeid(rhs);
  }

  static bool isDefaultSchema(const std::shared_ptr<Schema>& schema) {
    if (schema == nullptr) return false;
    static const std::shared_ptr<Schema> defaultSchema = Schema::createDefaultSchema();
    if (schema->fieldTypeMap.size() != defaultSchema->fieldTypeMap.size()) return false;
    for (const auto& [name, defaultField] : defaultSchema->fieldTypeMap) {
      auto it = schema->fieldTypeMap.find(name);
      if (it == schema->fieldTypeMap.end() || !fieldTypesEqual(*it->second, *defaultField)) return false;
    }
    return true;
  }

  // Extract owning copies of the (non-owning) response into the result; called from done(),
  // where the message (and its response arena) are still alive.
  static void fillResult(ProtoUpdateMessage& msg, IndexResult& out) {
    auto* rsp = msg.finishResponse();
    out.updateVersion = rsp->update_version;
    out.status = rsp->status;
    out.error_message = std::string(rsp->error_message);
    out.success = !msg.result.errored() && rsp->status != IndexResult::Status::ERROR;
    out.ids.clear();
    out.ids.reserve(rsp->ids.size());
    for (auto id : rsp->ids) out.ids.emplace_back(id);
    out.errors.clear();
    out.errors.reserve(rsp->errors.size());
    for (const auto& e : rsp->errors) {
      out.errors.push_back({std::string(e.id), std::string(e.error_message), e.index});
    }
  }

  // Run a fully-built (arena-backed) concrete UpdateRequest synchronously. `request` and its
  // backing arena must outlive this call (they do: the caller holds them on the stack).
  IndexResult runSync(const solux::api::UpdateRequest& request) {
    auto writer = collection().getShard()->getIndexWriter();

    class BlockingProtoUpdateMessage : public ProtoUpdateMessage {
    public:
      Blocker blocker;
      IndexResult* out;
      BlockingProtoUpdateMessage(const RequestProto* req, IndexResult* out)
        : ProtoUpdateMessage(req), out(out) {}
      void done(IndexWriter& iw) override {
        unused(iw);
        fillResult(*this, *out);
        blocker.notify();
      }
    };

    IndexResult result;
    BlockingProtoUpdateMessage msg(&request, &result);
    bool ok = writer->submitUpdate(&msg);
    assert(ok);
    unused(ok);
    msg.blocker.wait();
    return result;
  }

public:
  // Convert a Doc to a concrete (non-owning) Map, building all nested data into `mr` (which
  // must outlive any use of the resulting Map). The FieldVal arms map to Val variant arms.
  static void convertDocToProto(const Doc& doc, solux::api::Map& map, std::pmr::memory_resource& mr) {
    using namespace solux::api;
    using Pair = std::pair<std::string_view, ::hpp_proto::indirect_view<Val>>;
    Pair* fields = build::allocArray(map.fields, doc.size(), mr);
    std::size_t fi = 0;
    for (const auto& nv : doc) {
      Val* val = (Val*)mr.allocate(sizeof(Val), alignof(Val));
      new (val) Val();
      std::visit(overloaded{
        [&](bool v) { val->kind = v; },
        [&](int64_t v) { val->kind = v; },
        [&](float v) { val->kind = v; },
        [&](double v) { val->kind = v; },
        [&](const std::string& v) { val->kind = build::arenaStr(mr, v); },
        [&](const std::vector<bool>&) {
          // No bool-array wire encoding exists; fail loudly rather than silently drop the field.
          throw std::runtime_error("convertDocToProto: bool-array field values are not supported");
        },
        [&](const std::vector<int64_t>& v) {
          auto& arr = val->kind.emplace<ArrInt>();
          int64_t* a = build::allocArray(arr.v, v.size(), mr);
          std::copy(v.begin(), v.end(), a);
        },
        [&](const std::vector<float>& v) {
          // vector<float> is a single dense VECTOR field value in test inputs.
          auto& vec = val->kind.emplace<Vector>();
          auto& f32 = vec.f32.emplace();
          float* a = build::allocArray(f32.v, v.size(), mr);
          std::copy(v.begin(), v.end(), a);
        },
        [&](const std::vector<double>& v) {
          auto& arr = val->kind.emplace<ArrDouble>();
          double* a = build::allocArray(arr.v, v.size(), mr);
          std::copy(v.begin(), v.end(), a);
        },
        [&](const std::vector<std::string>& v) {
          auto& arr = val->kind.emplace<ArrStr>();
          std::string_view* a = build::allocArray(arr.v, v.size(), mr);
          for (std::size_t i = 0; i < v.size(); i++) a[i] = build::arenaStr(mr, v[i]);
        },
        [&](const std::vector<std::vector<float>>& v) {
          auto& arr = val->kind.emplace<ArrVector>();
          Vector* a = build::allocArray(arr.v, v.size(), mr);
          for (std::size_t i = 0; i < v.size(); i++) {
            auto& f32 = a[i].f32.emplace();
            float* fa = build::allocArray(f32.v, v[i].size(), mr);
            std::copy(v[i].begin(), v[i].end(), fa);
          }
        }
      }, nv.val);
      fields[fi++] = Pair{build::arenaStr(mr, nv.name), {val}};
    }
  }

  // Fluent builder for a concrete UpdateRequest, for cases the convenience methods don't
  // cover (all_or_none, return_ids, mixed adds + deletes). Owns its build arena, so it must
  // outlive the submit() call (it does: callers keep it on the stack). Build, then pass to
  // submit(). All nested doc/id data is copied into the builder's arena.
  class UpdateBuilder {
    std::pmr::monotonic_buffer_resource mr_;
    build::SpanBuilder<solux::api::Map> docs_{mr_};
    build::SpanBuilder<std::string_view> deleteIds_{mr_};
    solux::api::UpdateRequest request_;

  public:
    UpdateBuilder& add(const Doc& doc) {
      convertDocToProto(doc, docs_.emplace_back(), mr_);
      return *this;
    }
    UpdateBuilder& remove(std::string_view id) {
      deleteIds_.push_back(build::arenaStr(mr_, id));
      return *this;
    }
    UpdateBuilder& overwrite(bool v = true) { request_.allow_dups = !v; return *this; }
    UpdateBuilder& allowDups(bool v = true) { request_.allow_dups = v; return *this; }
    UpdateBuilder& allOrNone(bool v = true) { request_.all_or_none = v; return *this; }
    UpdateBuilder& returnIds(bool v = true) { request_.return_ids = v; return *this; }
    UpdateBuilder& collection(std::string_view name) {
      auto& t = request_.collection.emplace();
      std::string_view* a = build::allocArray(t.name, 1, mr_);
      a[0] = build::arenaStr(mr_, name);
      return *this;
    }
    UpdateBuilder& requestId(std::string_view id) {
      request_.request_id = build::arenaStr(mr_, id);
      return *this;
    }
    UpdateBuilder& streamId(int64_t id) { request_.stream_id = id; return *this; }
    UpdateBuilder& commit(bool waitForMerges = false) {
      auto& p = request_.commit.emplace();
      p.wait_for_merges = waitForMerges;
      return *this;
    }
    UpdateBuilder& commitWithAux(std::span<const std::string> names) {
      auto& params = request_.commit.emplace();
      std::string_view* a = build::allocArray(params.build_aux_indexes, names.size(), mr_);
      for (std::size_t i = 0; i < names.size(); i++) a[i] = build::arenaStr(mr_, names[i]);
      return *this;
    }

    // Seal the accumulated docs/ids into the request spans and return it (stable until this
    // builder is destroyed).
    const solux::api::UpdateRequest& finish() {
      request_.docs = docs_.finish();
      request_.delete_ids = deleteIds_.finish();
      return request_;
    }
  };

  CollectionHelper(std::string_view name = "main") {
    collection_ = SoluxTest::soluxNode->getOrCreateCollection(name);
    getIndexWriter()->mergePolicy->setMergeFactor(10);  // reset in case other tests forget.
  }

  Collection& collection() { return *collection_; }

  void commit(const std::vector<std::string>& buildAuxIndexes = {}) {
    auto writer = collection().getShard()->getIndexWriter();
    if (buildAuxIndexes.empty()) { writer->commit(); return; }

    UpdateBuilder b;
    b.commitWithAux(buildAuxIndexes);
    runSync(b.finish());
  }

  IndexResult index(const Doc& doc, UpdateMessage::CommitType commitType = UpdateMessage::NO_COMMIT, bool overwrite = false) {
    return indexAll({&doc, 1}, commitType, overwrite);
  }

  // Submit a fully-built UpdateBuilder (for cases the convenience methods don't cover:
  // all_or_none, return_ids, mixed adds + deletes); build docs with builder.add().
  IndexResult submit(UpdateBuilder& builder) { return runSync(builder.finish()); }

  // NOTE: distinct name (not an `index` overload) - std::span's initializer_list ctor would
  // make index({{"id","1"}}) ambiguous. Mirrors deleteById / deleteByIds.
  IndexResult indexAll(std::span<const Doc> docs, UpdateMessage::CommitType commitType = UpdateMessage::NO_COMMIT, bool overwrite = false) {
    UpdateBuilder b;
    for (const auto& doc : docs) b.add(doc);
    if (commitType != UpdateMessage::NO_COMMIT) b.commit();
    b.overwrite(overwrite);
    return runSync(b.finish());
  }

  IndexResult deleteByIds(std::span<const std::string> ids, UpdateMessage::CommitType commitType = UpdateMessage::NO_COMMIT) {
    UpdateBuilder b;
    for (const auto& id : ids) b.remove(id);
    if (commitType != UpdateMessage::NO_COMMIT) b.commit();
    return runSync(b.finish());
  }

  IndexResult deleteById(const std::string& id, UpdateMessage::CommitType commitType = UpdateMessage::NO_COMMIT) {
    return deleteByIds({&id, 1}, commitType);
  }

  // Async version of index.  Docs are copied into the update message's arena.
  void index(Doc&& doc, std::function<void(const IndexResult& result)>&& callback,
             UpdateMessage::CommitType commitType = UpdateMessage::NO_COMMIT, bool overwrite = false) {
    index(std::vector<Doc>{std::move(doc)}, std::move(callback), commitType, overwrite);
  }

  void index(std::vector<Doc>&& docs, std::function<void(const IndexResult& result)>&& callback,
             UpdateMessage::CommitType commitType = UpdateMessage::NO_COMMIT, bool overwrite = false) {
    auto writer = collection().getShard()->getIndexWriter();

    // Builds (and owns) the concrete request + its arena BEFORE the ProtoUpdateMessage base
    // ctor runs (it reads req->commit), so the builder is a base-from-member holder.
    struct ReqHolder {
      UpdateBuilder builder;
      ReqHolder(std::span<const Doc> docs, bool commit, bool overwrite) {
        for (const auto& doc : docs) builder.add(doc);
        if (commit) builder.commit();
        builder.overwrite(overwrite);
      }
    };

    class ProtoUpdateMessageWithCallback : ReqHolder, public ProtoUpdateMessage {
    public:
      std::function<void(const IndexResult& result)> callback;
      ProtoUpdateMessageWithCallback(std::span<const Doc> docs, bool commit, bool overwrite,
                                     std::function<void(const IndexResult& result)> cb)
        : ReqHolder(docs, commit, overwrite),
          ProtoUpdateMessage(&this->builder.finish()), callback(std::move(cb)) {}
      void done(IndexWriter& iw) override {
        unused(iw);
        IndexResult result;
        fillResult(*this, result);
        callback(result);
        delete this;  // frees the owned builder (and thus the request arena) after done()
      }
    };

    auto* msg = new ProtoUpdateMessageWithCallback(docs, commitType != UpdateMessage::NO_COMMIT,
                                                   overwrite, std::move(callback));
    bool ok = writer->submitUpdate(msg);
    assert(ok);
    unused(ok);
  }

  void clear() {
    auto writer = collection().getShard()->getIndexWriter();
    writer->testDeleteAllData();
    if (!isDefaultSchema(collection().getSchema())) {
      collection().setSchema(Schema::createDefaultSchema());
    }
  }

  std::shared_ptr<IndexWriter> getIndexWriter() { return collection().getShard()->getIndexWriter(); }

  SearchEngine& getSearchEngine() { return SoluxTest::soluxNode->getSearchEngine(); }

  /// Given nDocs, mergeFactor, and a base-36 "shape" string, fill docsPerSeg with
  /// per-segment doc counts (largest to smallest). For reusing a prior test/bench index.
  static void calcSegSizes(int64_t nDocs, int mergeFactor, std::string_view shape, std::vector<int32_t>& docsPerSeg) {
    docsPerSeg.clear();
    std::vector<int32_t> segsPerTier;
    for (char c : shape) {
      if (c > '9') segsPerTier.push_back(c - 'a' + 10);
      else segsPerTier.push_back(c - '0');
    }
    double totalWeight = 0.0;
    double currentLevelSize = 1;
    double effectiveMergeFactor = mergeFactor * 1.05;
    for (int i = shape.length() - 1; i >= 0; --i) {
      totalWeight += segsPerTier[i] * currentLevelSize;
      currentLevelSize *= effectiveMergeFactor;
    }
    if (totalWeight == 0) return;
    double scale = nDocs / totalWeight;
    int power = segsPerTier.size() - 1;
    for (auto nSegs : segsPerTier) {
      int32_t docs = scale * std::pow(effectiveMergeFactor, power);
      if (docs < 1) break;
      for (int i = 0; i < nSegs; ++i) docsPerSeg.push_back(docs);
      power--;
    }
    int64_t diff = nDocs - std::accumulate(docsPerSeg.begin(), docsPerSeg.end(), 0LL);
    if (diff > 0) {
      docsPerSeg[0] += diff;
    } else if (diff < 0) {
      docsPerSeg.back() += diff;
      assert(docsPerSeg.back() >= 0);
    }
  }

  // True if the current index matches the shape of docsPerSeg (for index reuse).
  bool indexMatchesShape(std::span<const int32_t> docsPerSeg) {
    auto iw = getIndexWriter();
    auto reader = iw->getIndexReader();
    auto readerSegs = reader->segments().size();
    bool reuseIndex = readerSegs == docsPerSeg.size();
    if (reuseIndex) {
      for (size_t i = 0; i < docsPerSeg.size(); i++) {
        auto& seg = reader->segments()[i];
        if (seg.postingsReader().maxDoc() != docsPerSeg[i]) { reuseIndex = false; break; }
      }
    }
    return reuseIndex;
  }
};

} // namespace solux::test
