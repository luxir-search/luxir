// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <optional>

#include "PostingsWriter.h"

namespace luxir {

// Writes the two doc bitsets of a BOOLEAN column (FieldType::BOOL_BITSETS):
// the docs with at least one true value and the docs with at least one
// false value. Values arrive in doc order; a doc may be added more than once
// (multi-valued columns) and lands in each set at most once. finish() fills
// SegFieldInfo::trueDocs/trueDocsEndLoc and falseDocs/falseDocsEndLoc with
// the docsWithField sentinels: a count of 0 or maxDoc has no bitset.
//
// The pool is only allocated from; the caller rewinds it after finish().
class BoolBitsetsWriter {
  class Set {
    MemPool& pool;
    PostingsWriter& postingsWriter;
    OutputStreamPtr output;
    std::optional<DocsWriter> docs;
    int32_t lastDoc = -1;
    int32_t count = 0;

  public:
    Set(MemPool& pool, PostingsWriter& postingsWriter)
      : pool(pool), postingsWriter(postingsWriter) {}

    void add(int32_t doc) {
      assert(doc >= lastDoc);
      if (doc == lastDoc) return;
      if (!docs) {
        // Reserve the stream on the first doc: a set nobody is in costs
        // nothing.
        output = postingsWriter.getOutputStream();
        docs.emplace(pool, *output);
      }
      docs->addDoc(doc);
      lastDoc = doc;
      count++;
    }

    void finish(int32_t maxDoc, int32_t& countOut, seg_location& endLoc) {
      countOut = count;
      endLoc = {0, 0};
      if (!docs) return;
      int32_t written = docs->finish();
      assert(written == count);
      unused(written);
      // Every doc: the bytes just written are dead weight until the next
      // merge drops them, the same trade DocsWithValWriter makes.
      if (count != maxDoc) endLoc = output->slocation();
    }
  };

  Set trueDocs;
  Set falseDocs;

public:
  BoolBitsetsWriter(MemPool& pool, PostingsWriter& postingsWriter)
    : trueDocs(pool, postingsWriter), falseDocs(pool, postingsWriter) {}

  // Docs in non-decreasing order. value is the column value (0 or 1).
  void add(int32_t doc, int64_t value) {
    if (value != 0) trueDocs.add(doc);
    else falseDocs.add(doc);
  }

  void finish(int32_t maxDoc, SegFieldInfo& fieldInfo) {
    trueDocs.finish(maxDoc, fieldInfo.trueDocs, fieldInfo.trueDocsEndLoc);
    falseDocs.finish(maxDoc, fieldInfo.falseDocs, fieldInfo.falseDocsEndLoc);
  }
};

} // namespace luxir
