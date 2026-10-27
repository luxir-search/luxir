// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "luxir/util/proto.h"
#include "ProtoUpdateMessage.h"
#include "LuxirNode.h"
#include "CommitWaits.h"
#include "luxir/index/IndexWriter.h"
#include "luxir/schema/Schema.h"

#include <boost/unordered/unordered_flat_map.hpp>
#include <new>
#include <charconv>
#include <optional>
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


// Hide the physical child-key encoding from document acknowledgements/errors.
static std::string_view responseDocId(const api::Map& doc, const FieldNameMap& fieldMap) {
  auto id = docId(doc, fieldMap);
  if (id.starts_with("\1luxir:")) {
    auto* child = doc.fields.find("luxir_child_s");
    if (child) return (*child)->asString();
  }
  return id;
}

static void update(ProtoUpdateMessage& msg, Inverter& inverter, const Inverter::UndoMark& requestMark) {
  auto& request = *msg.req;
  const bool allOrNone = request.all_or_none;
  const bool returnIds = request.return_ids;
  const FieldNameMap fieldMap(request);
  // Handlers cached by raw field position: a request's docs usually share a shape,
  // so a hit costs one name compare.
  std::vector<Inverter::InputHandler*> handlers;
  // The current doc's fields after last-wins dedup, in first-seen order.
  std::vector<std::pair<Inverter::InputHandler*, const IndexVal*>> fields;

  // first docid this request's adds will use; needed for all_or_none rollback.
  const int32_t firstDoc = inverter.getMaxDoc();

  int32_t failed = 0;
  int32_t docIndex = -1;
  for (const auto &doc : request.docs) {
    docIndex++;
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
        // new name (a derived selector is not a document key, and the engine
        // assigns _version_).
        if (handler == nullptr || *handler != *fname) {
          if (*fname == "_version_") {
            throw DocumentError("Field '_version_' is assigned by the engine; optimistic concurrency is not supported");
          }
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
      failed++;
      ErrorInfo info = currentExceptionInfo(ErrorKind::INTERNAL);
      if (info.kind == ErrorKind::INTERNAL) {
        LOG_ERROR("document {} (id '{}') failed to index: {}", docIndex, responseDocId(doc, fieldMap),
                  info.message);
      }
      auto* response = msg.getResponse();
      auto& err = msg.addError();
      err.id = luxir::api::build::arenaStr(msg.responseArena(), responseDocId(doc, fieldMap));
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
      msg.addId(responseDocId(doc, fieldMap));
    }
  }

  if (failed > 0) {
    // ERROR only if nothing in the request had any effect.
    bool anySuccess = failed < (int32_t)request.docs.size() || !request.delete_ids.empty();
    msg.getResponse()->status = anySuccess ? ProtoUpdateMessage::ResponseStatus::PARTIAL
                                           : ProtoUpdateMessage::ResponseStatus::ERROR;
  }
}


// A request with family mutations is one transaction in one inverter. All
// validation and expansion happens before deletes or postings are touched.
static std::string childKey(std::string_view family, std::string_view relation, std::string_view id) {
  return "\1luxir:" + std::to_string(family.size()) + ":" + std::string(family)
      + std::to_string(relation.size()) + ":" + std::string(relation) + std::string(id);
}

static api::UpdateRequest expandRelations(ProtoUpdateMessage& msg, std::pmr::memory_resource& mr,
    std::vector<api::Map>& docs, std::vector<std::string_view>& deletes) {
  auto request = *msg.req;
  FieldNameMap fieldMap(request);
  auto validateDoc = [&](const api::Map& doc) {
    for (const auto& [name, value] : doc.fields) {
      auto mapped = fieldMap.resolve(name);
      if (mapped && mapped->starts_with("luxir_")) throw RequestError("luxir_ fields are reserved for relations");
    }
    auto id = docId(doc, fieldMap);
    if (id.starts_with("\1luxir:")) throw RequestError("reserved internal document id");
  };
  for (const auto& doc : request.docs) validateDoc(doc);
  for (const auto& id : request.delete_ids) if (id.starts_with("\1luxir:")) throw RequestError("reserved internal document id");
  if (request.families.empty() && request.child_updates.empty()) return request;
  if (msg.schema->relations.empty()) throw RequestError("family updates require schema.relations");
  if (request.allow_dups || !request.field_map.empty() || request.drop_unmapped)
    throw RequestError("family updates require unique ids and do not support field_map");
  request.all_or_none = true;
  docs.assign(request.docs.begin(), request.docs.end());
  deletes.assign(request.delete_ids.begin(), request.delete_ids.end());
  auto add = [&](api::Map& doc, size_t capacity, std::string_view name, const api::Val& value) {
    *api::build::mapSlot(doc.fields, capacity, api::build::arenaStr(mr, name), mr) = value;
  };
  auto string = [&](std::string_view value) { api::Val v; v.kind = api::build::arenaStr(mr, value); return v; };
  auto indexChild = [&](const api::Map& child, std::string_view parent, std::string_view token, std::string_view relation) {
    validateDoc(child);
    auto id = docId(child, fieldMap);
    if (id.empty()) throw RequestError("each child requires a non-empty string id");
    api::Map physical;
    size_t capacity = child.fields.size() + 6;
    for (auto [field, val] : child.fields) {
      if (field == "id") continue;
      if (field == "_version_") throw RequestError("_version_ is assigned by the engine");
      add(physical, capacity, "luxir_rel_" + std::to_string(relation.size()) + "_" + std::string(relation) + "_" + std::string(field), *val);
    }
    add(physical, capacity, "id", string(childKey(token, relation, id)));
    add(physical, capacity, "luxir_relation_s", string(relation));
    add(physical, capacity, "luxir_family_s", string(token));
    add(physical, capacity, "luxir_parent_s", string(parent));
    add(physical, capacity, "luxir_child_s", string(id));
    std::string json;
    if (!api::write_json(child, json)) throw RequestError("cannot serialize child source");
    add(physical, capacity, "luxir_source_s", string(json));
    docs.push_back(physical);
  };
  using Pair = std::pair<std::string_view, std::string_view>;
  std::vector<Pair> tokens;
  std::set<std::string> parentIds;
  for (const auto& family : request.families) {
    if (!family.parent) throw RequestError("family requires parent");
    validateDoc(*family.parent);
    auto id = docId(*family.parent, fieldMap);
    if (id.empty() || !parentIds.insert(std::string(id)).second) throw RequestError("families require distinct non-empty parent ids");
    std::string token = std::to_string(msg.updateVersion) + ":" + std::string(id);
    api::Map parent;
    for (const auto& [name, value] : family.parent->fields) add(parent, family.parent->fields.size() + 1, name, *value);
    add(parent, family.parent->fields.size() + 1, "luxir_family_s", string(token));
    docs.push_back(parent);
    tokens.emplace_back(api::build::arenaStr(msg.responseArena(), id), api::build::arenaStr(msg.responseArena(), token));
    for (const auto& [relation, children] : family.children) {
      if (!msg.schema->relations.contains(relation)) throw RequestError("unknown relation: " + std::string(relation));
      std::set<std::string> childIds;
      for (const auto& value : children.v) {
        auto* child = std::get_if<api::Map>(&value.kind);
        if (!child) throw RequestError("family children must be objects");
        if (!childIds.insert(std::string(docId(*child, fieldMap))).second) throw RequestError("duplicate child id in family relation");
        indexChild(*child, id, token, relation);
      }
    }
  }
  for (const auto& update : request.child_updates) {
    if (!msg.schema->relations.contains(update.relation)) throw RequestError("unknown child update relation");
    auto colon = update.family.find(':');
    if (update.parent_id.empty() || colon == std::string_view::npos || colon == 0
        || update.family.substr(colon + 1) != update.parent_id
        || update.family.substr(0, colon).find_first_not_of("0123456789") != std::string_view::npos)
      throw RequestError("child_updates requires the family's incarnation token and matching parent_id");
    uint64_t version = 0;
    auto parsed = std::from_chars(update.family.data(), update.family.data() + colon, version);
    if (parsed.ec != std::errc{} || version >= msg.updateVersion)
      throw RequestError("child update token must come from an earlier family update");
    if (parentIds.contains(std::string(update.parent_id))) throw RequestError("cannot replace a family and mutate its children together");
    for (auto id : update.delete_ids) deletes.push_back(api::build::arenaStr(mr, childKey(update.family, update.relation, id)));
    for (const auto& child : update.docs) indexChild(child, update.parent_id, update.family, update.relation);
  }
  if (!tokens.empty()) {
    auto* out = (Pair*)msg.responseArena().allocate(sizeof(Pair) * tokens.size(), alignof(Pair));
    std::uninitialized_copy(tokens.begin(), tokens.end(), out);
    msg.getResponse()->families = api::map_view<std::string_view, std::string_view>(std::span<const Pair>(out, tokens.size()));
  }
  request.docs = docs;
  request.delete_ids = deletes;
  return request;
}


void ProtoUpdateMessage::handle(IndexWriter& iw) {
  std::pmr::monotonic_buffer_resource expansionArena;
  std::vector<api::Map> expandedDocs;
  std::vector<std::string_view> expandedDeletes;
  auto expanded = expandRelations(*this, expansionArena, expandedDocs, expandedDeletes);
  const auto* original = req;
  req = &expanded;
  auto restore = scope_guard([&]() {
    req = original;
    if (getResponse()->status == ResponseStatus::ERROR) getResponse()->families = {};
  });

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
