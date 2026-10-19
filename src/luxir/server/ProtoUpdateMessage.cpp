// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "luxir/util/proto.h"
#include "ProtoUpdateMessage.h"
#include "LuxirNode.h"
#include "CommitWaits.h"
#include "luxir/index/IndexWriter.h"
#include "luxir/schema/Schema.h"
#include "luxir/schema/ValCoerce.h"

#include <algorithm>
#include <boost/unordered/unordered_flat_map.hpp>
#include <deque>
#include <iterator>
#include <new>
#include <oneapi/tbb/blocked_range.h>
#include <oneapi/tbb/parallel_for.h>
#include <oneapi/tbb/task_arena.h>
#include <optional>
#include <ranges>
#include <variant>

namespace luxir {

using IndexVal = luxir::api::Val;
using BytesView = ::hpp_proto::bytes_view;


// The request's field_map collapsed to protobuf last-wins entries: input doc key ->
// schema field, "" = drop. Views alias the request, which outlives update processing.
class FieldNameMap {
  boost::unordered_flat_map<std::string_view, std::string_view> entries;
  bool dropUnmapped;

public:
  explicit FieldNameMap(const luxir::api::UpdateRequest& req) : dropUnmapped(req.drop_unmapped) {
    for (const auto& [from, to] : lastWins(req.field_map)) {
      entries.emplace(from, *to);
    }
  }

  // The schema field to index the input key under, or nullopt to drop the field.
  std::optional<std::string_view> resolve(std::string_view name) const {
    if (entries.empty()) {
      return name;  // drop_unmapped without a map is rejected at validation
    }
    auto iter = entries.find(name);
    if (iter == entries.end()) {
      if (dropUnmapped) return std::nullopt;
      return name;
    }
    if (iter->second.empty()) return std::nullopt;
    return iter->second;
  }
};


// Rejects a bad field_map once at the request level, before any inverter work, so it
// surfaces as one message-level error rather than repeating on every doc.
static void validateFieldMap(const luxir::api::UpdateRequest& req) {
  if (req.drop_unmapped && req.field_map.empty()) {
    throw RequestError("drop_unmapped requires a non-empty field_map");
  }
  for (const auto& [from, to] : req.field_map) {
    unused(from);
    if (!to.empty() && !Schema::validFieldName(to)) {
      throw RequestError("field_map target is not a valid field name: " + std::string(to));
    }
  }
}


// The value of the doc's unique id field, or empty if not present.
// "id" is the schema-defined name for the unique id field.
static std::string_view docId(const luxir::api::Map& doc, const FieldNameMap& fieldMap) {
  std::string_view result;
  for (const auto& [name, valView] : doc.fields) {
    if (fieldMap.resolve(name) != std::string_view("id")) {
      continue;
    }
    const IndexVal& val = *valView;
    if (const auto* s = std::get_if<std::string_view>(&val.kind)) {
      result = *s;
    } else if (const auto* bin = std::get_if<BytesView>(&val.kind)) {
      result = std::string_view((const char*)bin->data(), bin->size());
    } else {
      result = {};
    }
  }
  return result;  // last "id" wins (protobuf map dedup semantics)
}


// Indexes the request docs at `indices` (ascending request positions) into one
// inverter.  A document's own failure is reported through
// onFailure(docIndex, ErrorInfo&&); returning false rolls the doc back and
// continues, true stops the batch (all_or_none: the caller rolls back the whole
// request).  onSuccess(docIndex) reports an indexed doc.  Segment-fatal failures
// propagate to the caller, which must discard the inverter.
template <class Indices, class OnFailure, class OnSuccess>
static void indexDocs(Inverter& inverter, const luxir::api::UpdateRequest& request,
                      const FieldNameMap& fieldMap, const Indices& indices,
                      OnFailure&& onFailure, OnSuccess&& onSuccess) {
  // Handlers cached by raw field position: a request's docs usually share a shape,
  // so a hit costs one name compare.
  std::vector<Inverter::InputHandler*> handlers;
  // The current doc's fields after last-wins dedup, in first-seen order.
  std::vector<std::pair<Inverter::InputHandler*, const IndexVal*>> fields;
  for (int32_t docIndex : indices) {
    const auto& doc = request.docs[(size_t)docIndex];
    size_t nFields = doc.fields.size();
    if (handlers.size() < nFields) {
      handlers.resize(nFields);
    }

    auto docMark = inverter.undoMark();
    inverter.startDoc();

    try {
      // Duplicate keys post-mapping collapse last-wins (protobuf map semantics). The
      // handler is a name's identity, so its claim finds an earlier occurrence without
      // a per-doc map; a repeat replaces the value at the first occurrence's position.
      fields.clear();
      size_t raw = 0;
      for (const auto& [name, val] : doc.fields) {
        Inverter::InputHandler*& handler = handlers[raw++];
        auto fname = fieldMap.resolve(name);
        if (!fname) continue;
        // A cached handler proves its name was accepted; creation validates a
        // new name (a derived selector is not a document key).
        if (handler == nullptr || *handler != *fname) {
          handler = &inverter.getIndexHandler(*fname);
        }
        uint32_t slot = handler->claim(inverter, (uint32_t)fields.size());
        if (slot == fields.size()) fields.emplace_back(handler, &*val);
        else fields[slot].second = &*val;
      }
      for (const auto& [handler, val] : fields) {
        handler->index(inverter, *val);
      }

      inverter.finishDoc();
    } catch (const FileIOException&) {
      // A direct column/stored-fields write may have appended only a prefix.
      // The inverter must be discarded, not recovered as a document error.
      throw;
    } catch (const std::bad_alloc&) {
      // Output buffering can allocate after bytes have reached the file, so an
      // allocation failure has the same segment-fatal policy.
      throw;
    } catch (...) {
      // Segment-fatal failures were rethrown above.  A document's own faults
      // announce themselves (DocumentError, RequestError from the handlers);
      // anything else is an engine fault, reported on the document it hit and
      // logged, while the document itself is rolled back like any other.
      ErrorInfo info = currentExceptionInfo(ErrorKind::INTERNAL);
      if (info.kind == ErrorKind::INTERNAL) {
        LOG_ERROR("document {} (id '{}') failed to index: {}", docIndex, docId(doc, fieldMap),
                  info.message);
      }
      if (onFailure(docIndex, std::move(info))) {
        return;
      }

      // Undo this doc's id map mutations and mark the partially indexed doc as
      // deleted, then continue with the next doc.
      inverter.rollbackTo(docMark);
      inverter.deleteDoc(inverter.getDoc());
      continue;
    }

    onSuccess(docIndex);
  }
}


static void addDocError(ProtoUpdateMessage& msg, const FieldNameMap& fieldMap, int32_t docIndex,
                        const ErrorInfo& info) {
  auto& err = msg.addError();
  err.id = luxir::api::build::arenaStr(msg.responseArena(), docId(msg.req->docs[(size_t)docIndex], fieldMap));
  err.index = docIndex;
  err.error = luxir::api::build::arenaError(msg.responseArena(), info);
}


static void setFailureStatus(ProtoUpdateMessage& msg, size_t failed) {
  if (failed == 0) return;
  // ERROR only if nothing in the request had any effect.
  bool anySuccess = failed < msg.req->docs.size() || !msg.req->delete_ids.empty();
  msg.getResponse()->status = anySuccess ? ProtoUpdateMessage::ResponseStatus::PARTIAL
                                         : ProtoUpdateMessage::ResponseStatus::ERROR;
}


static void update(ProtoUpdateMessage& msg, Inverter& inverter, const Inverter::UndoMark& requestMark) {
  auto& request = *msg.req;
  const bool allOrNone = request.all_or_none;
  const bool returnIds = request.return_ids;
  const FieldNameMap fieldMap(request);

  // first docid this request's adds will use; needed for all_or_none rollback.
  const int32_t firstDoc = inverter.getMaxDoc();

  size_t failed = 0;
  bool aborted = false;
  indexDocs(inverter, request, fieldMap, std::views::iota(0, (int32_t)request.docs.size()),
      [&](int32_t docIndex, ErrorInfo&& info) {
        failed++;
        addDocError(msg, fieldMap, docIndex, info);
        if (!allOrNone) return false;
        // Undo the id map mutations of the whole request (including queued
        // delete_ids) and mark every doc it added as deleted.  Remaining docs
        // are not attempted.
        inverter.rollbackTo(requestMark);
        for (int32_t docid = firstDoc; docid <= inverter.getDoc(); docid++) {
          inverter.deleteDoc(docid);
        }
        msg.clearIds();
        msg.getResponse()->status = ProtoUpdateMessage::ResponseStatus::ERROR;
        aborted = true;
        return true;
      },
      [&](int32_t docIndex) {
        if (returnIds) {
          msg.addId(docId(request.docs[(size_t)docIndex], fieldMap));
        }
      });

  if (!aborted) {
    setFailureStatus(msg, failed);
  }
}


// Releases an inverter on all paths. If an exception escapes document-level
// recovery, the append-only segment may contain a partial value. Mark it
// failed and send it through the flush pipeline for commit accounting and
// cleanup, but never reuse or publish it. The message-level catch in
// processUpdateBody reports the error.
// On the success path, flushOnRelease (set once at the end of the batch)
// asks releaseInverter to flush the inverter to a segment if it has grown past
// its cap.
struct ReleaseGuard {
  IndexWriter& iw;
  Inverter& inverter;
  bool flushOnRelease = false;
  std::exception_ptr failure = nullptr;
  ~ReleaseGuard() {
    if (failure != nullptr) {
      inverter.fail(failure);
      iw.releaseInverter(inverter, true);
      return;
    }
    iw.releaseInverter(inverter, flushOnRelease);
  }
};


// Counts the messages inside handle() so concurrent large messages share the
// arena instead of each splitting to its full width.
struct InHandle {
  std::atomic<int32_t>& count;
  int32_t value;
  explicit InHandle(std::atomic<int32_t>& c) : count(c), value(c.fetch_add(1) + 1) {}
  ~InHandle() { count.fetch_sub(1); }
};


// The overwrite key IdHandler will index for this doc's id, or nullopt when the
// doc has no id or its id is rejected (the doc then fails on its own).  Mirrors
// IdHandler::index: string and bytes pass through, other scalars render through
// the id field type, and the result is fitted to the term limit.  Rendered or
// hashed keys are kept alive in `owned`; others view the request.
static std::optional<std::string_view> overwriteKey(const luxir::api::Map& doc, const FieldNameMap& fieldMap,
                                                    const FieldType& idType, std::deque<std::string>& owned) {
  const IndexVal* val = nullptr;
  for (const auto& [name, valView] : doc.fields) {
    if (fieldMap.resolve(name) == std::string_view("id")) {
      val = &*valView;  // last "id" wins, as in docId()
    }
  }
  if (val == nullptr || coerce::isNull(*val)) return std::nullopt;

  std::string_view id;
  if (const auto* s = std::get_if<std::string_view>(&val->kind)) {
    id = *s;
  } else if (const auto* bin = std::get_if<BytesView>(&val->kind)) {
    id = std::string_view((const char*)bin->data(), bin->size());
  } else {
    char buf[coerce::TEXT_BUF_SIZE];
    try {
      id = owned.emplace_back(idType.coerceTerm(*val, "id", buf));
    } catch (...) {
      return std::nullopt;
    }
  }
  PackedTerm::TermBuffer scratch;
  auto fitted = PackedTerm::fitTerm(idType.longTerms, id, scratch);
  if (!fitted) return std::nullopt;
  // A hashed key lives in scratch; a passthrough or truncated one views id.
  if (fitted->data() < id.data() || fitted->data() >= id.data() + id.size()) {
    return owned.emplace_back(*fitted);
  }
  return fitted;
}


// Assigns every doc to one of nChunks chunks, each an ascending list of request
// positions.  Chunks are contiguous ranges, except that with overwrite every doc
// sharing an id joins the chunk of that id's first occurrence.  All docs of one
// request share one updateVersion, and a cross-inverter overwrite delete only
// supersedes STRICTLY lower versions, so two copies of an id in different
// inverters would both stay live.  In one inverter IdHandler resolves them in
// request order exactly as the unsplit path does, including a failed later copy
// leaving the earlier one in place.
static std::vector<std::vector<int32_t>> partitionDocs(const luxir::api::UpdateRequest& request,
                                                       const FieldNameMap& fieldMap, const Schema& schema,
                                                       int32_t nChunks) {
  const int32_t nDocs = (int32_t)request.docs.size();
  const int32_t chunkSize = (nDocs + nChunks - 1) / nChunks;
  std::vector<std::vector<int32_t>> chunks((size_t)nChunks);
  for (auto& chunk : chunks) chunk.reserve((size_t)chunkSize);

  const FieldType* idType = request.allow_dups ? nullptr : schema.getFieldTypePtr("id");
  if (idType == nullptr) {
    for (int32_t i = 0; i < nDocs; i++) chunks[(size_t)(i / chunkSize)].push_back(i);
    return chunks;
  }

  std::deque<std::string> owned;
  boost::unordered_flat_map<std::string_view, int32_t> chunkOfId;
  chunkOfId.reserve((size_t)nDocs);
  for (int32_t i = 0; i < nDocs; i++) {
    int32_t chunk = i / chunkSize;
    if (auto key = overwriteKey(request.docs[(size_t)i], fieldMap, *idType, owned)) {
      chunk = chunkOfId.try_emplace(*key, chunk).first->second;
    }
    chunks[(size_t)chunk].push_back(i);
  }
  return chunks;
}


// Indexes one message across nChunks inverters in parallel.  Per-doc outcomes are
// buffered per chunk (the response arena is not thread safe) and folded into the
// response in request order afterwards, so errors, ids and status read exactly as
// the single-inverter path would produce them.
static void updateSplit(ProtoUpdateMessage& msg, IndexWriter& iw, const Schema& schema, int32_t nChunks) {
  const auto& request = *msg.req;
  const FieldNameMap fieldMap(request);
  auto chunkDocs = partitionDocs(request, fieldMap, schema, nChunks);

  struct DocFailure {
    int32_t docIndex;
    ErrorInfo info;
  };
  std::vector<std::vector<DocFailure>> failures((size_t)nChunks);
  std::vector<uint8_t> indexed(request.docs.size(), 0);  // each slot written by one chunk only

  std::vector<std::optional<ReleaseGuard>> guards((size_t)nChunks);
  for (auto& guard : guards) {
    Inverter& inverter = iw.obtainInverter(msg.updateVersion, msg.schema);
    guard.emplace(iw, inverter);
    inverter.overwrite = !request.allow_dups;
    inverter.coerceContext.dateMathNowEpochMillis = msg.dateMathNowEpochMillis;
  }

  // delete_ids go to the first chunk's inverter.  They delete versions below this
  // message's, so a doc re-added under a deleted id in any chunk survives, as in
  // the single-inverter path.
  {
    Inverter& first = guards[0]->inverter;
    auto requestMark = first.undoMark();
    try {
      for (const auto& id : request.delete_ids) {
        first.deleteId(id, msg.updateVersion);
      }
    } catch (const DocumentError&) {
      first.rollbackTo(requestMark);
      throw;
    } catch (...) {
      guards[0]->failure = std::current_exception();
      throw;
    }
  }

  // Isolated so a worker waiting here only runs this message's chunks, never an
  // unrelated flow-graph task (a commit or flush body) that could wait on this
  // message in turn.  A segment-fatal failure in one chunk fails only that chunk's
  // inverter; parallel_for cancels unstarted chunks and rethrows it here.
  oneapi::tbb::this_task_arena::isolate([&] {
    oneapi::tbb::parallel_for(
        oneapi::tbb::blocked_range<int32_t>(0, nChunks, 1),
        [&](const oneapi::tbb::blocked_range<int32_t>& range) {
          for (int32_t c = range.begin(); c != range.end(); c++) {
            ReleaseGuard& guard = *guards[(size_t)c];
            auto& chunkFailures = failures[(size_t)c];
            try {
              indexDocs(guard.inverter, request, fieldMap, chunkDocs[(size_t)c],
                  [&](int32_t docIndex, ErrorInfo&& info) {
                    chunkFailures.push_back({docIndex, std::move(info)});
                    return false;
                  },
                  [&](int32_t docIndex) { indexed[(size_t)docIndex] = 1; });
              // See the end-of-batch flush check in handle().
              guard.flushOnRelease = guard.inverter.shouldFlush(iw.perInverterRamBytes, iw.perInverterMaxDocs);
            } catch (...) {
              guard.failure = std::current_exception();
              throw;
            }
          }
        },
        oneapi::tbb::simple_partitioner());
  });

  std::vector<DocFailure> allFailures;
  for (auto& chunk : failures) {
    std::move(chunk.begin(), chunk.end(), std::back_inserter(allFailures));
  }
  std::sort(allFailures.begin(), allFailures.end(),
            [](const DocFailure& a, const DocFailure& b) { return a.docIndex < b.docIndex; });
  for (const auto& failure : allFailures) {
    addDocError(msg, fieldMap, failure.docIndex, failure.info);
  }
  if (request.return_ids) {
    for (size_t i = 0; i < indexed.size(); i++) {
      if (indexed[i]) msg.addId(docId(request.docs[i], fieldMap));
    }
  }
  setFailureStatus(msg, allFailures.size());
}


// Chunks to split this message into; 1 keeps it in a single inverter.
static int32_t splitChunks(const IndexWriter& iw, const luxir::api::UpdateRequest& request, int32_t inHandle) {
  // all_or_none needs one undo scope to roll the whole request back.
  if (request.all_or_none || iw.parallelUpdateChunkDocs == 0) return 1;
  size_t byDocs = request.docs.size() / iw.parallelUpdateChunkDocs;
  if (byDocs < 2) return 1;
  int32_t width = oneapi::tbb::this_task_arena::max_concurrency();
  int32_t share = std::max(1, width / std::max(1, inHandle));
  return (int32_t)std::min<size_t>({byDocs, (size_t)share, (size_t)std::max(1, iw.parallelUpdateMaxChunks)});
}


void ProtoUpdateMessage::handle(IndexWriter& iw) {
  // Counts the batch for the node and its collection as it leaves, however
  // it leaves. A message with no documents and no delete ids (a commit-only
  // message) is not a batch; its commit counts under the COMMIT stage.
  struct Count {
    ProtoUpdateMessage& msg;
    Metrics& node;
    CollectionMetrics& collection;
    bool active = !msg.req->docs.empty() || !msg.req->delete_ids.empty();
    int exceptions = std::uncaught_exceptions();
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    Count(ProtoUpdateMessage& msg, Metrics& node, CollectionMetrics& collection)
        : msg(msg), node(node), collection(collection) {
      if (active) node.start(metricActivity(MetricStage::INDEX_BATCH));
    }
    ~Count() {
      if (!active) return;
      const auto& req = *msg.req;
      // The batch failed when nothing in it applied; a partial batch's
      // rejections count per document.
      bool failed = std::uncaught_exceptions() > exceptions || msg.metricFailed();
      auto outcome = failed ? MetricOutcome::ERROR : MetricOutcome::OK;
      auto nanos = nanosSince(start);
      auto errors = msg.docErrors();
      uint64_t rejected = failed ? req.docs.size() : errors.size();
      uint64_t accepted = req.docs.size() - rejected;
      uint64_t deleteIds = failed ? 0 : req.delete_ids.size();
      node.complete(metricActivity(MetricStage::INDEX_BATCH), outcome, nanos);
      node.add(MetricCounter::DOCUMENTS_ACCEPTED, accepted);
      node.add(MetricCounter::DOCUMENTS_REJECTED, rejected);
      node.add(MetricCounter::DELETE_IDS_ACCEPTED, deleteIds);
      for (const auto& error : errors) {
        node.documentError(error.error ? (ErrorKind)error.error->kind : ErrorKind::UNKNOWN);
      }
      collection.batch(outcome, nanos, accepted, rejected, deleteIds);
    }
  } count{*this, *iw.metrics, *iw.collectionMetrics};
  validateFieldMap(*req);

  // Check if we need an inverter for either deletes or adds
  bool needInverter = !req->delete_ids.empty() || !req->docs.empty();

  if (!needInverter) {
    return;
  }

  InHandle inHandle(iw.updatesInHandle);
  int32_t nChunks = splitChunks(iw, *req, inHandle.value);
  if (nChunks > 1) {
    auto pinned = this->schema ? this->schema : iw.getSchema();
    updateSplit(*this, iw, *pinned, nChunks);
    return;
  }

  Inverter& inverter = iw.obtainInverter(this->updateVersion, this->schema);
  ReleaseGuard releaseGuard{iw, inverter};
  inverter.overwrite = !req->allow_dups;
  inverter.coerceContext.dateMathNowEpochMillis = dateMathNowEpochMillis;

  // Captured before deletes are queued so an all_or_none failure rolls them back too.
  auto requestMark = inverter.undoMark();

  // Delete validation is recoverable and has no partial postings to discard.
  // Roll back this request's queued ids on rejection, preserving documents
  // already accumulated by earlier messages in the same inverter.
  try {
    // Process deletes before adds (shouldn't matter since we just queue deletes)
    if (!req->delete_ids.empty()) {
      for (const auto& id : req->delete_ids) {
        inverter.deleteId(id, this->updateVersion);
      }
    }
  } catch (const DocumentError&) {
    inverter.rollbackTo(requestMark);
    throw;
  } catch (...) {
    releaseGuard.failure = std::current_exception();
    throw;
  }

  try {
    // Process document additions
    if (!req->docs.empty()) {
      update(*this, inverter, requestMark);
    }

    // Size-based auto-flush is checked ONCE here, at the end of the batch - never
    // mid-request.  The docs this message put in one inverter stay in it, which
    // keeps within-request id overwrites correct: IdHandler resolves a repeated id
    // in memory by directly deleting the superseded doc.  Splitting the copies of an
    // id across a mid-flush segment boundary (or across inverters) would break that -
    // every doc in one message shares one updateVersion, and a cross-segment overwrite
    // delete only supersedes docs with a STRICTLY lower version (finishCommitBody
    // gates on seg.minVersion < maxDeleteVersion), so a duplicate id straddling the
    // split could not delete its earlier, same-version copy and both would stay live.
    // (A large message split across inverters by updateSplit keeps every copy of an
    // id in one chunk for this reason.)  The cost: a single message larger than the
    // cap is not bounded here (bound those with a max-message-size reject - separate
    // follow-up).  A non-stop stream is byte-batched into many messages that
    // accumulate in one reused idle inverter, so the per-message check still bounds
    // the streaming OOM this feature targets.
    releaseGuard.flushOnRelease = inverter.shouldFlush(iw.perInverterRamBytes, iw.perInverterMaxDocs);
  } catch (...) {
    releaseGuard.failure = std::current_exception();
    throw;
  }
}


std::shared_ptr<ProtoUpdateMessage::Completion> ProtoUpdateMessage::takeCompletion(LuxirNode& node) {
  auto completed = std::make_shared<Completion>();
  completed->response = *finishResponse();
  auto errors = completed->response.errors;
  auto* copiedErrors = api::build::allocArray(completed->response.errors, errors.size(), *mr_);
  std::copy(errors.begin(), errors.end(), copiedErrors);
  auto ids = completed->response.ids;
  auto* copiedIds = api::build::allocArray(completed->response.ids, ids.size(), *mr_);
  std::copy(ids.begin(), ids.end(), copiedIds);
  completed->commit = resultingCommit;
  if (resultingCommit && req->commit && req->commit->wait_for_replicas && !result.errored()) {
    auto& waits = node.getCommitWaits();
    auto collection = target.name.empty() ? LuxirNode::target({}, req->collection) : target;
    completed->wait = waits.prepareReplicas(std::move(collection), *resultingCommit, *req->commit->wait_for_replicas,
        waits.deadlineAfter(req->commit->wait_for_replicas_timeout_ms.value_or(30000)));
  }
  completed->arena = std::move(mr_);
  req = nullptr;
  return completed;
}

void ProtoUpdateMessage::Completion::await(LuxirNode& node,
    std::function<void(std::shared_ptr<Completion>)> delivery, std::stop_token stop) {
  auto self = shared_from_this();
  if (!wait) { delivery(std::move(self)); return; }
  auto& waits = node.getCommitWaits();
  waits.awaitReplicas(std::move(*wait), stop,
      [self, delivery = std::move(delivery)](api::ReplicaResult replicas) {
        self->response.replicas = replicas;
        delivery(self);
      });
}

} // namespace luxir
