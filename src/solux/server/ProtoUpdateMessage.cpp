#include "solux/util/proto.h"
#include "ProtoUpdateMessage.h"
#include "solux/index/IndexWriter.h"

#include <variant>

namespace solux {

using IndexVal = solux::api::Val;
using BytesView = ::hpp_proto::bytes_view;


// Message for the in-flight exception, including non-std exceptions.
static std::string currentExceptionMessage() {
  try {
    throw;
  } catch (const std::exception& e) {
    return e.what();
  } catch (...) {
    return "unknown non-standard exception";
  }
}


// The value of the doc's unique id field, or empty if not present.
// "id" is the schema-defined name for the unique id field.
static std::string_view docId(const solux::api::Map& doc) {
  std::string_view result;
  for (const auto& [name, valView] : doc.fields) {
    if (name != "id") {
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


// Adds are indexed into *inverterPtr.  A non-atomic request may flush the current
// inverter mid-stream (auto-flush) and swap in a fresh one; inverterPtr, requestMark,
// and firstDoc are repointed at the new inverter so the caller's ReleaseGuard follows.
static void update(ProtoUpdateMessage& msg, IndexWriter& iw, Inverter*& inverterPtr,
                   Inverter::UndoMark& requestMark, int32_t& firstDoc, uint64_t updateVersion) {
  auto& request = *msg.req;
  const bool allOrNone = request.all_or_none;
  const bool returnIds = request.return_ids;
  std::vector<Inverter::IndexHandler*> handlers;

  int32_t failed = 0;
  int32_t docIndex = -1;
  for (const auto &doc : request.docs) {
    docIndex++;
    Inverter& inverter = *inverterPtr;  // current inverter (rebinds after a mid-request flush)
    size_t nFields = doc.fields.size();
    if (handlers.size() < nFields) {
      handlers.resize(nFields);
    }

    auto docMark = inverter.undoMark();
    inverter.startDoc();

    try {
      int idx = 0;
      for (const auto& [fname, fval] : lastWins(doc.fields)) {  // dedup duplicate field keys, last-wins
        auto handler = handlers[idx];
        if (handler == nullptr || *handler != fname) {
          handlers[idx] = handler = &inverter.getIndexHandler(fname);
        }

        handler->index(inverter, **fval);
        idx++;
      }

      inverter.finishDoc();
    } catch (...) {
      failed++;
      auto* response = msg.getResponse();
      auto& err = msg.addError();
      err.id = solux::api::build::arenaStr(msg.responseArena(), docId(doc));
      err.index = docIndex;
      err.error_message = solux::api::build::arenaStr(msg.responseArena(), currentExceptionMessage());

      if (allOrNone) {
        // Undo the id map mutations of the whole request (including queued
        // delete_ids) and mark every doc it added as deleted.  Remaining docs
        // are not attempted.  all_or_none never auto-flushes, so firstDoc/requestMark
        // still refer to this one inverter.
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
      msg.addId(docId(doc));
    }

    // Auto-flush at a safe doc boundary to bound the RAM one request holds (e.g. a
    // non-stop stream).  Only non-atomic requests may flush mid-request: an
    // all_or_none request must keep every doc in a single inverter so a later
    // failure can roll the whole request back.  A flush ships the current inverter
    // to its own segment (its docs become durable at the next commit) and swaps in
    // a fresh one; the cached handlers belong to the old inverter, so drop them and
    // let the next doc re-resolve against the new inverter.
    if (!allOrNone && inverter.shouldFlush(iw.perInverterRamBytes, iw.perInverterMaxDocs)) {
      iw.releaseInverter(inverter, /*flush=*/true);
      inverterPtr = &iw.obtainInverter(updateVersion);
      inverterPtr->overwrite = !request.allow_dups;
      requestMark = inverterPtr->undoMark();
      firstDoc = inverterPtr->getMaxDoc();
      for (auto& h : handlers) h = nullptr;
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
  // Check if we have columns (not yet implemented)
  if (req->columns.has_value()) {
    std::cout << "\tindexer got columns (not yet implemented!): " << req->columns->columns.size() << std::endl;
    return;
  }

  // Check if we need an inverter for either deletes or adds
  bool needInverter = !req->delete_ids.empty() || !req->docs.empty();

  if (!needInverter) {
    return;
  }

  // The current inverter for this request.  A non-atomic request may auto-flush
  // mid-stream, which ships this inverter to a segment and swaps in a fresh one;
  // `inverter`, `requestMark`, and `firstDoc` are then repointed at the new
  // inverter, and the ReleaseGuard below follows them by reference.
  Inverter* inverter = &iw.obtainInverter(this->updateVersion);
  inverter->overwrite = !req->allow_dups;

  // Captured before deletes are queued so an all_or_none failure rolls them back too.
  auto requestMark = inverter->undoMark();
  // first docid this request's adds will use in the current inverter; the rollback
  // baseline for both the guard and the all_or_none path.
  int32_t firstDoc = inverter->getMaxDoc();

  // Releases the current inverter on all paths.  If an exception escapes doc
  // processing (per-doc recovery catches everything, so realistically only
  // allocation failure), also roll that inverter back so no partially indexed doc
  // is left live; the message-level catch in processUpdateBody reports the error.
  // After a mid-request flush, earlier docs are already durable in their own
  // segment - only the current inverter's docs are rolled back.
  struct ReleaseGuard {
    IndexWriter& iw;
    Inverter*& inverter;            // current inverter (may be swapped by an auto-flush)
    Inverter::UndoMark& requestMark;
    int32_t& firstDoc;
    // compare against the count at construction so an unrelated in-flight
    // exception (e.g. during TBB graph teardown) doesn't look like ours
    int uncaughtOnEntry = std::uncaught_exceptions();
    ~ReleaseGuard() {
      if (std::uncaught_exceptions() > uncaughtOnEntry) {
        try {
          inverter->rollbackTo(requestMark);
          for (int32_t docid = firstDoc; docid <= inverter->getDoc(); docid++) {
            inverter->deleteDoc(docid);
          }
        } catch (...) {
          // deleteDoc can allocate; swallow rather than terminate during unwind.
          LOG_ERROR("Rollback after update exception failed; partially indexed docs may remain live.");
        }
      }
      iw.releaseInverter(*inverter);
    }
  } releaseGuard{iw, inverter, requestMark, firstDoc};

  // Process deletes before adds (shouldn't matter since we just queue deletes)
  if (!req->delete_ids.empty()) {
    for (const auto& id : req->delete_ids) {
      inverter->deleteId(id, this->updateVersion);
    }
  }

  // Process document additions
  if (!req->docs.empty()) {
    update(*this, iw, inverter, requestMark, firstDoc, this->updateVersion);
  }
}

} // namespace solux
