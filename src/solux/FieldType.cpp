
#include "index/SegField.h"
#include "FieldType.h"

SegFieldIndexed* FieldType::createSegFieldIndexed(MemPool& pool) {
  if (flags_ & INDEX_DOCS_AND_FREQS_AND_POSITIONS) {
    return new SegFieldDocsFreqPos(*this, pool);
  }
  assert(false);  // not implemented yet, or should null mean "don't index?"
  // TODO: should related fields be linked up here? return a set of fields?
  return nullptr;
}