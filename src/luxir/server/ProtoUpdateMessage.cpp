// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "luxir/util/proto.h"
#include "ProtoUpdateMessage.h"
#include "luxir/index/IndexWriter.h"
#include "luxir/schema/Schema.h"

#include <boost/unordered/unordered_flat_map.hpp>
#include <new>
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


static void update(ProtoUpdateMessage& msg, Inverter& inverter, const Inverter::UndoMark& requestMark) {
  auto& request = *msg.req;
  const bool allOrNone = request.all_or_none;
  const bool returnIds = request.return_ids;
  const FieldNameMap fieldMap(request);
  std::vector<Inverter::InputHandler*> handlers;

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
      int idx = 0;
      // dedup duplicate field keys post-mapping, last-wins
      for (const auto& [fname, fval] :
           lastWins(doc.fields, [&](std::string_view name) { return fieldMap.resolve(name); })) {
        if (fname.find("__") != std::string_view::npos) {
          throw DocumentError("Derived field is not a logical document key: " + std::string(fname),
                              "invalid_field_name");
        }
        auto handler = handlers[idx];
        if (handler == nullptr || *handler != fname) {
          handlers[idx] = handler = &inverter.getIndexHandler(fname);
        }

        handler->index(inverter, **fval);
        idx++;
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
        LOG_ERROR("document {} (id '{}') failed to index: {}", docIndex, docId(doc, fieldMap),
                  info.message);
      }
      auto* response = msg.getResponse();
      auto& err = msg.addError();
      err.id = luxir::api::build::arenaStr(msg.responseArena(), docId(doc, fieldMap));
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
      msg.addId(docId(doc, fieldMap));
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

  try {
    // Process deletes before adds (shouldn't matter since we just queue deletes)
    if (!req->delete_ids.empty()) {
      for (const auto& id : req->delete_ids) {
        inverter.deleteId(id, this->updateVersion);
      }
    }

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

} // namespace luxir
