#include "ProtoUpdateMessage.h"
#include "solux/index/IndexWriter.h"

namespace solux {


static void update(ProtoUpdateMessage& msg, IndexWriter& iw, Inverter& inverter) {
  unused(iw);
  auto& request = *msg.req;
  std::vector<Inverter::IndexHandler*> handlers;
  Inverter::IndexHandler* versionHandler = nullptr;
  
  // Get version handler once if overwrite is enabled
  if (request.overwrite()) {
    versionHandler = &inverter.getIndexHandler("_version_");
  }

  if (request.docs_size() > 0) {
    for (const auto &doc : request.docs()) {
      size_t nFields = doc.fields_size();
      if (handlers.size() < nFields) {
        handlers.resize(nFields);
      }

      inverter.startDoc();

      // TODO: wrap in try/catch here for recovery of individual doc fails?

      int idx = 0;
      for (const auto&[fname, fval] : doc.fields()) {
        auto handler = handlers[idx];
        if (handler == nullptr || *handler != fname) {
          handlers[idx] = handler = &inverter.getIndexHandler(fname);
        }
        handler->index(inverter, fval);
        idx++;
      }

      // Add _version_ field when overwrite is enabled
      if (versionHandler != nullptr) {
        versionHandler->index(inverter, static_cast<int64_t>(msg.updateVersion));
      }

      inverter.finishDoc();
    }
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

  // TODO: FIXME: if we hit an exception here, we still want to release the inverter! Use a guard like a
  // unique_ptr with a custom deleter.
  Inverter& inverter = iw.obtainInverter(this->updateVersion);

  // Process deletes before adds (shouldn't matter since we just queue deletes)
  if (req->delete_ids_size() > 0) {
    for (const auto& id : req->delete_ids()) {
      inverter.deleteId(id, this->updateVersion);
    }
  }

  // Process document additions
  if (req->docs_size() > 0) {
    update(*this, iw, inverter);
  }
  
  iw.releaseInverter(inverter);
}

} // namespace solux