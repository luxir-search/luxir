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


static void update(ProtoUpdateMessage& msg, Inverter& inverter, const Inverter::UndoMark& requestMark) {
  auto& request = *msg.req;
  const bool allOrNone = request.all_or_none;
  const bool returnIds = request.return_ids;
  std::vector<Inverter::IndexHandler*> handlers;

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
      msg.addId(docId(doc));
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

  Inverter& inverter = iw.obtainInverter(this->updateVersion);
  inverter.overwrite = !req->allow_dups;
  inverter.coerceContext.dateMathNowEpochMillis = dateMathNowEpochMillis;

  // Captured before deletes are queued so an all_or_none failure rolls them back too.
  auto requestMark = inverter.undoMark();

  // Releases the inverter on all paths.  If an exception escapes doc processing
  // (per-doc recovery catches everything, so realistically only allocation
  // failure), also roll the whole request back so no partially indexed doc is
  // left live; the message-level catch in processUpdateBody reports the error.
  // On the success path, flushOnRelease (set once at the end of the batch below)
  // asks releaseInverter to flush the inverter to a segment if it has grown past
  // its cap.  On the exception path we release without flushing; the next message
  // re-checks the size.
  struct ReleaseGuard {
    IndexWriter& iw;
    Inverter& inverter;
    Inverter::UndoMark requestMark;
    int32_t firstDoc;
    bool flushOnRelease = false;
    // compare against the count at construction so an unrelated in-flight
    // exception (e.g. during TBB graph teardown) doesn't look like ours
    int uncaughtOnEntry = std::uncaught_exceptions();
    ~ReleaseGuard() {
      if (std::uncaught_exceptions() > uncaughtOnEntry) {
        try {
          inverter.rollbackTo(requestMark);
          for (int32_t docid = firstDoc; docid <= inverter.getDoc(); docid++) {
            inverter.deleteDoc(docid);
          }
        } catch (...) {
          // deleteDoc can allocate; swallow rather than terminate during unwind.
          LOG_ERROR("Rollback after update exception failed; partially indexed docs may remain live.");
        }
        iw.releaseInverter(inverter);
        return;
      }
      iw.releaseInverter(inverter, flushOnRelease);
    }
  } releaseGuard{iw, inverter, requestMark, inverter.getMaxDoc()};

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
}

} // namespace solux
