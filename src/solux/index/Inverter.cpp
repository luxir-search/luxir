
#include "Inverter.h"

namespace solux {
void Inverter::index(Document &doc) {
  unused(doc);
  /*
  int docid = ++currDoc_;
  for (auto fv : doc.fields) {
    indexField(fv);
  }
  */
}


void Inverter::indexField(FieldValue &fv) {
  unused(fv);
}

} // end namespace
