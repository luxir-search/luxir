#include "ProtoUpdateMessage.h"
#include "solux/index/IndexWriter.h"

namespace solux {


static void update(solux::proto::UpdateRequest& request, IndexWriter& iw, Inverter& inverter) {
  unused(iw);
  std::vector<Inverter::IndexHandler*> handlers;
  // int ndocs = request->docs_size();

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

      inverter.finishDoc();
    }
  }
}


void ProtoUpdateMessage::handle(IndexWriter& iw) {
  // if there are no docs, then we can just return before grabbing an inverter.
  if (req->docs_size() == 0) {
    if (req->has_columns()) {
      std::cout << "\tindexer got columns (not yet implemented!): " << req->columns().columns_size() << std::endl;
    }
    return;
  }

  // TODO: FIXME: if we hit an exception here, we still want to release the inverter! Use a guard like a
  // unique_ptr with a custom deleter.
  Inverter& inverter = iw.obtainInverter();
  update(*req, iw, inverter);
  iw.releaseInverter(inverter);
}

} // namespace solux