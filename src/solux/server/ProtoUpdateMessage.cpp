#include "ProtoUpdateMessage.h"
#include "solux/index/IndexWriter.h"

namespace solux {


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
static std::string_view docId(const proto::Map& doc) {
  auto it = doc.fields().find("id");
  if (it == doc.fields().end()) {
    return {};
  }
  const proto::Val& val = it->second;
  if (val.has_s()) {
    return val.s();
  }
  if (val.has_bin()) {
    return val.bin();
  }
  return {};
}


static void update(ProtoUpdateMessage& msg, Inverter& inverter, const Inverter::UndoMark& requestMark) {
  auto& request = *msg.req;
  const bool allOrNone = request.all_or_none();
  const bool returnIds = request.return_ids();
  std::vector<Inverter::IndexHandler*> handlers;

  // first docid this request's adds will use; needed for all_or_none rollback.
  const int32_t firstDoc = inverter.getMaxDoc();

  int32_t failed = 0;
  int32_t docIndex = -1;
  for (const auto &doc : request.docs()) {
    docIndex++;
    size_t nFields = doc.fields_size();
    if (handlers.size() < nFields) {
      handlers.resize(nFields);
    }

    auto docMark = inverter.undoMark();
    inverter.startDoc();

    try {
      int idx = 0;
      for (const auto&[fname, fval] : doc.fields()) {
        auto handler = handlers[idx];
        if (handler == nullptr || *handler != fname) {
          handlers[idx] = handler = &inverter.getIndexHandler(fname);
        }

        handler->index(inverter, fval);
        idx++;
      }

      inverter.finishDoc();
    } catch (...) {
      failed++;
      auto* response = msg.getResponse();
      auto* err = response->add_errors();
      err->set_id(std::string(docId(doc)));
      err->set_index(docIndex);
      err->set_error_message(currentExceptionMessage());

      if (allOrNone) {
        // Undo the id map mutations of the whole request (including queued
        // delete_ids) and mark every doc it added as deleted.  Remaining docs
        // are not attempted.
        inverter.rollbackTo(requestMark);
        for (int32_t docid = firstDoc; docid <= inverter.getDoc(); docid++) {
          inverter.deleteDoc(docid);
        }
        response->clear_ids();
        response->set_status(proto::UpdateResponse::ERROR);
        return;
      }

      // Undo this doc's id map mutations and mark the partially indexed doc as
      // deleted, then continue with the next doc.
      inverter.rollbackTo(docMark);
      inverter.deleteDoc(inverter.getDoc());
      continue;
    }

    if (returnIds) {
      msg.getResponse()->add_ids(std::string(docId(doc)));
    }
  }

  if (failed > 0) {
    // ERROR only if nothing in the request had any effect.
    bool anySuccess = failed < request.docs_size() || request.delete_ids_size() > 0;
    msg.getResponse()->set_status(anySuccess ? proto::UpdateResponse::PARTIAL
                                             : proto::UpdateResponse::ERROR);
  }
}


void ProtoUpdateMessage::handle(IndexWriter& iw) {
  // Check if we have columns (not yet implemented)
  if (req->has_columns()) {
    std::cout << "\tindexer got columns (not yet implemented!): " << req->columns().columns_size() << std::endl;
    return;
  }

  // Check if we need an inverter for either deletes or adds
  bool needInverter = req->delete_ids_size() > 0 || req->docs_size() > 0;

  if (!needInverter) {
    return;
  }

  Inverter& inverter = iw.obtainInverter(this->updateVersion);
  inverter.overwrite = req->overwrite();

  // Captured before deletes are queued so an all_or_none failure rolls them back too.
  auto requestMark = inverter.undoMark();

  // Releases the inverter on all paths.  If an exception escapes doc processing
  // (per-doc recovery catches everything, so realistically only allocation
  // failure), also roll the whole request back so no partially indexed doc is
  // left live; the message-level catch in processUpdateBody reports the error.
  struct ReleaseGuard {
    IndexWriter& iw;
    Inverter& inverter;
    Inverter::UndoMark requestMark;
    int32_t firstDoc;
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
      }
      iw.releaseInverter(inverter);
    }
  } releaseGuard{iw, inverter, requestMark, inverter.getMaxDoc()};

  // Process deletes before adds (shouldn't matter since we just queue deletes)
  if (req->delete_ids_size() > 0) {
    for (const auto& id : req->delete_ids()) {
      inverter.deleteId(id, this->updateVersion);
    }
  }

  // Process document additions
  if (req->docs_size() > 0) {
    update(*this, inverter, requestMark);
  }
}

} // namespace solux
