// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "gtest/gtest.h"
#include "luxir/codec/PackedTail.h"
#include "luxir/util/random.h"

#include <algorithm>
#include <climits>
#include <cstring>
#include <memory>
#include <vector>

using namespace luxir;

namespace {

// Round trip of every doc and tf width with every value count, from both
// gap bases (a term's first block and a tail after full blocks).  Each
// encoding is decoded from a heap copy of exactly its size plus OVERREAD, so
// ASan flags any read past that slack.
TEST(PackedTailTest, roundTripsEveryWidthAndCount) {
  Rng rng(7);
  std::vector<uint8_t> scratch(PackedTail::MAX_BYTES);
  for (uint32_t docBits = 0; docBits <= 31; docBits++) {
    for (uint32_t tfBits : {0u, 1u, 6u, 7u, 13u, 30u, 31u}) {
      for (uint32_t n = 1; n <= PackedTail::MAX_VALUES; n += (n < 10 ? 1 : 13)) {
        for (uint32_t prevDoc : {UINT32_MAX, 1000u}) {
          // A 31-bit gap reaches INT32_MAX only from the -1 base, as the
          // term's only doc.
          if (docBits == 31 && (n > 1 || prevDoc != UINT32_MAX)) continue;
          std::vector<int32_t> docs(n);
          std::vector<int32_t> tfs(n);
          uint32_t prev = prevDoc;
          for (uint32_t i = 0; i < n; i++) {
            // The first gap is the width's maximum; later ones stay small so
            // the docs fit in int32.
            uint32_t gap = docBits == 0 ? 0 : (i == 0 ? (uint32_t) ((1ull << docBits) - 1)
                                                      : (uint32_t) rng.rint(std::min(1u << std::min(docBits, 30u), 1u << 16)));
            prev = prev + 1 + gap;
            docs[i] = (int32_t) prev;
            // The largest tf - 1 of a width; for 31 bits, INT32_MAX - 1.
            uint32_t maxTf = tfBits == 31 ? (uint32_t) INT32_MAX - 1 : (1u << tfBits) - 1;
            uint32_t tf = tfBits == 0 ? 0 : (i == n - 1 ? maxTf : (uint32_t) rng.rint(maxTf));
            tfs[i] = (int32_t) (tf + 1);
          }
          uint32_t bytes = PackedTail::encode(docs.data(), tfs.data(), n, prevDoc, scratch.data());
          auto encoded = std::make_unique<uint8_t[]>(bytes + PackedTail::OVERREAD);
          memcpy(encoded.get(), scratch.data(), bytes);
          PackedTail::Header header = PackedTail::readHeader(encoded.get());
          ASSERT_EQ(header.docBits, docBits);
          ASSERT_EQ(header.tfBits, tfBits);
          ASSERT_EQ(header.encodedBytes(n), bytes);
          ASSERT_EQ(header.headerBytes, tfBits < PackedTail::TF_ESCAPE ? 1u : 2u);
          ASSERT_EQ(bytes, header.headerBytes + (n * (docBits + tfBits) + 7) / 8);

          std::vector<int32_t> decodedDocs(n);
          std::vector<int32_t> decodedTfs(n);
          PackedTail::decodeDocs(encoded.get(), header, n, prevDoc, decodedDocs.data());
          PackedTail::decodeTfs(encoded.get(), header, n, decodedTfs.data());
          ASSERT_EQ(decodedDocs, docs) << docBits << " " << tfBits << " " << n;
          ASSERT_EQ(decodedTfs, tfs) << docBits << " " << tfBits << " " << n;
          ASSERT_EQ(PackedTail::lastDoc(encoded.get(), header, n, prevDoc), docs.back());
          if (prevDoc == UINT32_MAX) {
            ASSERT_EQ(PackedTail::firstDoc(encoded.get()), docs[0]);
          }
        }
      }
    }
  }
}

// Docs-only tails and consecutive docs cost the widths byte alone.
TEST(PackedTailTest, constantStreamsStoreNoBits) {
  std::vector<int32_t> docs;
  for (int32_t doc = 0; doc < 100; doc++) docs.push_back(doc);
  std::vector<int32_t> ones(docs.size(), 1);
  uint8_t encoded[PackedTail::MAX_BYTES + PackedTail::OVERREAD];
  EXPECT_EQ(1u, PackedTail::encode(docs.data(), nullptr, 100, UINT32_MAX, encoded));
  EXPECT_EQ(1u, PackedTail::encode(docs.data(), ones.data(), 100, UINT32_MAX, encoded));
  std::vector<int32_t> decoded(docs.size());
  PackedTail::decodeDocs(encoded, PackedTail::readHeader(encoded), 100, UINT32_MAX,
                         decoded.data());
  EXPECT_EQ(decoded, docs);
}

}  // namespace
