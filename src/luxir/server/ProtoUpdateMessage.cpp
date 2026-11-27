// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "luxir/util/proto.h"
#include "ProtoUpdateMessage.h"
#include "LuxirNode.h"
#include "CommitWaits.h"
#include "luxir/index/IndexWriter.h"
#include "luxir/index/ObjectFields.h"
#include "luxir/schema/Schema.h"
#include "luxir/schema/ValCoerce.h"
#include "luxir/source/SourceFilter.h"
#include "luxir/source/SourceJson.h"
#include "luxir/source/SourcePatch.h"

#include <boost/unordered/unordered_flat_set.hpp>
#include <fmt/format.h>

#include <boost/unordered/unordered_flat_map.hpp>
#include <memory_resource>
#include <new>
#include <optional>
#include <stdexcept>
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
  // Every key indexes under its own name.
  bool identity() const { return entries.empty(); }

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


// The value of the doc's unique id field as the index sees it, or empty if not
// present. "id" is the schema-defined name for the unique id field. A numeric
// id renders canonically into `buf`, the same text IdHandler indexes, so
// {"id": 123} names the document "123" here too.
static std::string_view docId(const luxir::api::Map& doc, const FieldNameMap& fieldMap, const FieldType& idType,
                              std::span<char> buf) {
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
    } else if (coerce::isNull(val)) {
      result = {};
    } else {
      // Error reporting calls this too, so a value that is no id (a list or a
      // map) yields an empty id here; indexing rejects it on its own.
      try {
        result = idType.coerceTerm(val, "id", buf);
      } catch (const std::exception&) {
        result = {};
      }
    }
  }
  return result;  // last "id" wins (protobuf map dedup semantics)
}


// Rejects patches that cannot be resolved, before the message takes effect.
static void validatePatches(const luxir::api::UpdateRequest& req, const Schema& schema) {
  using Op = luxir::api::PatchOp::Op;
  using C = luxir::api::PatchCondition::Op;
  if (req.patches.empty()) return;
  if (!schema.source.enabled) throw RequestError("patches need a collection that stores _source_");
  if (req.all_or_none) throw RequestError("patches cannot be combined with all_or_none");
  if (req.allow_dups) throw RequestError("patches cannot be combined with allow_dups");
  if (!req.field_map.empty() || req.drop_unmapped) {
    throw RequestError("patches cannot be combined with field_map or drop_unmapped");
  }
  if (!req.source_includes.empty() || !req.source_excludes.empty()) {
    throw RequestError("patches cannot be combined with source_includes or source_excludes");
  }
  if (req.commit && (req.commit->max_segments > 0 || req.commit->wait_for_merges ||
                     !req.commit->build_aux_indexes.empty())) {
    throw RequestError("patches cannot be combined with max_segments, wait_for_merges or build_aux_indexes");
  }
  boost::unordered_flat_set<std::string_view> ids;
  for (size_t i = 0; i < req.patches.size(); i++) {
    const auto& patch = req.patches[i];
    auto fail = [&](std::string_view why) {
      throw RequestError(fmt::format("patch {} (id '{}'): {}", i, patch.id, why));
    };
    if (patch.id.empty()) fail("no id");
    if (patch.delete_doc && !patch.ops.empty()) fail("delete_doc takes no ops");
    if (patch.delete_doc && (patch.upsert || patch.upsert_doc)) fail("delete_doc cannot upsert");
    if (patch.exists == luxir::api::Patch::Exists::MUST && (patch.upsert || patch.upsert_doc)) {
      fail("exists MUST cannot upsert");
    }
    for (const auto& op : patch.ops) {
      bool isMap = op.value.has_value() && std::holds_alternative<luxir::api::Map>(op.value->kind);
      if (op.op != Op::UNSET && !op.value.has_value()) fail(fmt::format("op on '{}' has no value", op.path));
      if (op.op == Op::REPLACE) {
        if (!op.path.empty()) fail("replace takes no path");
        if (!isMap) fail("replace takes an object value");
        continue;
      }
      if (op.op == Op::MERGE) {
        if (!isMap) fail("merge takes an object value");
        if (op.path.empty()) continue;
      }
      if (auto bad = source::invalidPatchPath(op.path)) fail(*bad);
    }
    for (const auto& cond : patch.when) {
      if (cond.path.empty()) fail("a condition has no path");
      if (cond.op != C::EXISTS && cond.op != C::MISSING && !cond.value.has_value()) {
        fail(fmt::format("condition on '{}' has no value", cond.path));
      }
    }
    ids.insert(patch.id);
  }
  // A document and a patch of one id in one request would share a version,
  // which leaves no order between them.
  for (const auto& id : req.delete_ids) {
    if (ids.contains(id)) throw RequestError(fmt::format("id '{}' is both deleted and patched", id));
  }
  const FieldNameMap fieldMap(req);
  const FieldType& idType = *schema.getFieldTypeEx("id");
  char idBuf[coerce::TEXT_BUF_SIZE];
  for (const auto& doc : req.docs) {
    auto id = docId(doc, fieldMap, idType, idBuf);
    if (!id.empty() && ids.contains(id)) {
      throw RequestError(fmt::format("id '{}' is both indexed and patched", id));
    }
  }
}


static void update(ProtoUpdateMessage& msg, Inverter& inverter, const Inverter::UndoMark& requestMark) {
  auto& request = *msg.req;
  const bool allOrNone = request.all_or_none;
  const bool returnIds = request.return_ids;
  const FieldNameMap fieldMap(request);
  const FieldType& idType = *inverter.schema->getFieldTypeEx("id");
  char idBuf[coerce::TEXT_BUF_SIZE];
  auto idOf = [&](const luxir::api::Map& doc) { return docId(doc, fieldMap, idType, idBuf); };
  ObjectFields objectFields;

  // first docid this request's adds will use; needed for all_or_none rollback.
  const int32_t firstDoc = inverter.getMaxDoc();

  // Collections that store their source keep each document as it was submitted:
  // the JSON bytes the transport retained, else the document as a serialized Map.
  // Source includes/excludes (the request's, else the collection's) narrow it.
  const bool keepSource = inverter.schema->source.enabled;
  const bool rawAligned = msg.rawDocs.size() == request.docs.size();
  std::optional<source::SourceFilter> requestFilter;
  if (!request.source_includes.empty() || !request.source_excludes.empty()) {
    try {
      requestFilter.emplace(request.source_includes, request.source_excludes);
    } catch (const std::invalid_argument& e) {
      throw RequestError(e.what());
    }
  }
  const source::SourceFilter* sourceFilter = requestFilter ? &*requestFilter : &inverter.schema->source.filter;
  if (sourceFilter->empty()) sourceFilter = nullptr;
  std::string sourceJson;
  std::string filteredJson;
  std::vector<std::byte> sourceMap;
  std::pmr::monotonic_buffer_resource filterArena;

  int32_t failed = 0;
  int32_t docIndex = -1;
  for (const auto &doc : request.docs) {
    docIndex++;

    auto docMark = inverter.undoMark();
    inverter.startDoc();

    try {
      if (keepSource) {
        uint8_t flags = request.allow_dups ? source::kFlagAllowDups : 0;
        // A narrowed body must never pass for the whole document (a rebuild from
        // source would silently lose the dropped paths).
        if (sourceFilter) flags |= source::kFlagFiltered;
        // Renamed keys would make the body name the wrong fields.
        if (!fieldMap.identity()) flags |= source::kFlagMapped;
        std::string_view body;
        bool asJson = rawAligned;
        if (asJson) {
          sourceJson.clear();
          source::minifyJson(msg.rawDocs[(size_t)docIndex], sourceJson);
          body = sourceJson;
          if (sourceFilter) {
            filteredJson.clear();
            asJson = sourceFilter->filterJson(sourceJson, filteredJson);
            body = filteredJson;
          }
        }
        if (asJson) {
          flags |= source::kFlagMinifiedJson;
        } else {
          const luxir::api::Map* kept = &doc;
          luxir::api::Map filtered;
          if (sourceFilter) {
            filterArena.release();
            filtered = sourceFilter->filterMap(doc, filterArena);
            kept = &filtered;
          }
          sourceMap.clear();
          if (!luxir::api::encode(*kept, sourceMap)) throw std::runtime_error("failed to serialize document source");
          body = std::string_view((const char*)sourceMap.data(), sourceMap.size());
          flags |= source::kFlagProtoMap;
        }
        inverter.addSource(idOf(doc), body, msg.updateVersion, flags);
      }
      objectFields.index(inverter, doc, fieldMap);

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
      failed++;
      ErrorInfo info = currentExceptionInfo(ErrorKind::INTERNAL);
      if (info.kind == ErrorKind::INTERNAL) {
        LOG_ERROR("document {} (id '{}') failed to index: {}", docIndex, idOf(doc), info.message);
      }
      auto* response = msg.getResponse();
      auto& err = msg.addError();
      err.id = luxir::api::build::arenaStr(msg.responseArena(), idOf(doc));
      err.index = docIndex;
      err.error = luxir::api::build::arenaError(msg.responseArena(), info);

      if (allOrNone) {
        // Undo the id map mutations of the whole request (including queued
        // delete_ids) and mark every doc it added as deleted.  Remaining docs
        // are not attempted.
        inverter.rollbackTo(requestMark);
        for (int32_t docid = firstDoc; docid <= inverter.getDoc(); docid++) {
          inverter.deleteDoc(docid);
        }
        msg.clearIds();
        response->status = ProtoUpdateMessage::ResponseStatus::ERROR;
        return;
      }

      // Undo this doc's id map mutations and mark the partially indexed doc as
      // deleted, then continue with the next doc.
      inverter.rollbackTo(docMark);
      inverter.deleteDoc(inverter.getDoc());
      continue;
    }

    if (returnIds) {
      msg.addId(idOf(doc));
    }
  }

  if (failed > 0) {
    // ERROR only if nothing in the request had any effect.
    bool anySuccess = failed < (int32_t)request.docs.size() || !request.delete_ids.empty();
    msg.getResponse()->status = anySuccess ? ProtoUpdateMessage::ResponseStatus::PARTIAL
                                           : ProtoUpdateMessage::ResponseStatus::ERROR;
  }
}


void ProtoUpdateMessage::handle(IndexWriter& iw) {
  // Counts the batch for the node and its collection as it leaves, however
  // it leaves. A message with no documents and no delete ids (a commit-only
  // message) is not a batch; its commit counts under the COMMIT stage.
  struct Count {
    ProtoUpdateMessage& msg;
    Metrics& node;
    CollectionMetrics& collection;
    bool active = !msg.req->docs.empty() || !msg.req->delete_ids.empty() || !msg.req->patches.empty();
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
  validatePatches(*req, *schema);

  // Check if we need an inverter for either deletes or adds
  bool needInverter = !req->delete_ids.empty() || !req->docs.empty();

  if (!needInverter) {
    return;
  }

  Inverter& inverter = iw.obtainInverter(this->updateVersion, this->schema);
  inverter.overwrite = !req->allow_dups;
  inverter.coerceContext.dateMathNowEpochMillis = dateMathNowEpochMillis;

  // Captured before deletes are queued so an all_or_none failure rolls them back too.
  auto requestMark = inverter.undoMark();

  // Releases the inverter on all paths. If an exception escapes document-level
  // recovery, the append-only segment may contain a partial value. Mark it
  // failed and send it through the flush pipeline for commit accounting and
  // cleanup, but never reuse or publish it. The message-level catch in
  // processUpdateBody reports the error.
  // On the success path, flushOnRelease (set once at the end of the batch below)
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
  } releaseGuard{iw, inverter};

  // Delete validation is recoverable and has no partial postings to discard.
  // Roll back this request's queued ids on rejection, preserving documents
  // already accumulated by earlier messages in the same inverter.
  try {
    if (!Inverter::rowsFit((uint64_t)inverter.getMaxDoc(), req->docs.size())) {
      releaseGuard.flushOnRelease = true;
      throw DocumentError("Update exceeds the root row limit in the current buffer");
    }
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
    // mid-request.  A whole update message stays in a single inverter, which keeps
    // within-request id overwrites correct: IdHandler resolves a repeated id in
    // memory by directly deleting the superseded doc.  Splitting a request across a
    // mid-flush segment boundary would break that - every doc in one message shares
    // one updateVersion, and a cross-segment overwrite delete only supersedes docs
    // with a STRICTLY lower version (finishCommitBody gates on
    // seg.minVersion < maxDeleteVersion), so a duplicate id straddling the split
    // could not delete its earlier, same-version copy and both would stay live.
    // Keeping the request whole avoids that.  The cost: a single message larger than
    // the cap is not bounded here (bound those with a max-message-size reject -
    // separate follow-up).  A non-stop stream is byte-batched into many messages that
    // accumulate in one reused idle inverter, so the per-message check still bounds
    // the streaming OOM this feature targets.
    releaseGuard.flushOnRelease = inverter.shouldFlush(iw.perInverterRamBytes, iw.perInverterMaxDocs);
  } catch (...) {
    releaseGuard.failure = std::current_exception();
    throw;
  }
}


// Runs on the follow-up message that indexes this message's patched
// documents, with the inverter already at this message's update version.
void ProtoUpdateMessage::indexPatchedDocs(IndexWriter& iw, Inverter& inverter) {
  unused(iw);
  const FieldNameMap fieldMap(*req);
  ObjectFields objectFields;
  const bool keepSource = inverter.schema->source.enabled;
  std::pmr::monotonic_buffer_resource arena;
  for (const auto& doc : patchedDocs) {
    auto fail = [&](const std::string& message) {
      for (uint32_t index : doc.patchIndexes) {
        auto& outcome = patchOutcomes[index];
        outcome.status = luxir::api::PatchResult::Status::ERROR;
        outcome.version = 0;
        outcome.error = message;
      }
    };
    if (doc.deleted) {
      inverter.deleteId(doc.id, updateVersion);
      continue;
    }
    arena.release();
    luxir::api::Map map;
    std::string err;
    if (!luxir::api::read_json(map, doc.body, arena, &err)) {
      fail("the patched document does not parse: " + err);
      continue;
    }
    auto mark = inverter.undoMark();
    inverter.startDoc();
    try {
      if (keepSource) inverter.addSource(doc.id, doc.body, updateVersion, source::kFlagMinifiedJson);
      objectFields.index(inverter, map, fieldMap);
      inverter.finishDoc();
    } catch (const FileIOException&) {
      throw;
    } catch (const std::bad_alloc&) {
      throw;
    } catch (...) {
      // The patched document does not index: the current one stays.
      ErrorInfo info = currentExceptionInfo(ErrorKind::INTERNAL);
      inverter.rollbackTo(mark);
      inverter.deleteDoc(inverter.getDoc());
      fail(info.message);
    }
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
