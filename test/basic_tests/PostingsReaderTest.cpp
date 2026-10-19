// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "luxir/reader/FieldReader.h"
#include "luxir/index/PostingsWriter.h"
#include "luxir/reader/PostingsReader.h"
#include "gtest/gtest.h"
#include "test/LuxirTest.h"
#include "test/SegmentTest.h"
#include "test/CollectionHelper.h"
#include <barrier>
#include <thread>
#include <tuple>
#include<boost/container/static_vector.hpp>

namespace luxir {

class PostingsReaderTest : public LuxirTest {
protected:
  SegmentTest st;
public:

  void testTermSeek(int nTerms) {
    int nFields = rng.rint(1,4);
    st.positionsPerDocMax = 3;
    st.docsPerTermMax = 3;
    // st.termsPerFieldMax = Postings::TERMS_BLOCK_SIZE * 5 / 2;

    st.initWriter();
    st.addFields(false, nFields, nTerms, -1, -1);
    st.initReader();
    st.addFields(true, nFields, nTerms, -1, -1);

    bool found = st.tenum->seek("\0\0\0\0before beginning");
    ASSERT_EQ(false, found);

    // seekCeil of a target after every term has no ceil and reports exhausted.
    {
      std::string pastEnd;
      st.makeTerm(nTerms - 1, pastEnd);
      pastEnd.push_back('!');  // sorts after the last term
      ASSERT_FALSE(st.tenum->seekCeil(pastEnd));
    }
    // seekCeil of a target before every term positions on the first term.  Run
    // this last so the enum is left at ord 0 for the loop below.
    ASSERT_TRUE(st.tenum->seekCeil(""));
    ASSERT_EQ(st.tenum->ord(), 0);

    // random term lookups
    std::string fname;
    std::string missing;
    std::string term;
    // Track the current enum position so we can mix in seekForward (forward-only)
    // when the next target is >= the current term.  curOrd == -1 means fresh enum;
    // curOrd == nTerms means "position unknown after a miss" (disables seekForward
    // until the next successful (re)positioning).  This exercises seekForward the
    // same way applyDeletes does: a sorted run of forward seeks to present terms,
    // interleaved with full seeks.
    int curOrd = -1;
    for (int i=0; i < nTerms*nFields*2; i++) {
      if (rng.rint(0,10) == 0) { // 10% of the time, switch fields.
        st.getFieldName(fname, rng.rint(0, nFields));
        if (rng.rbool()) { // sometimes try to find a field that doesn't exist first
          missing = fname;
          missing.push_back('!');
          if (rng.rbool()) {  // sometimes change first char to test before / after lookup failures
            missing[0] = (char) rng.rbyte();
          }
          bool found = st.reader->findOrdinal(missing) >= 0;
          ASSERT_FALSE(found);
        }

        auto* info = st.reader->fieldInfo(fname);
        bool found = info != nullptr;
        ASSERT_TRUE(found);
        st.makeTermsEnum(*info);  // refresh the terms enum st.tenum
        curOrd = -1;
      }

      int tnum = rng.rint(0, nTerms);

      // Use seekForward when the target is at/ahead of the current position
      // (the forward-only contract).  curOrd == -1 means a fresh enum.
      bool canForward = (curOrd < 0) || (tnum >= curOrd);
      if (canForward && rng.rint(100) < 50) {
        if (rng.rbool()) {
          // forward seek to a PRESENT term at ord tnum
          st.makeTerm(tnum, term);
          bool found = st.tenum->seekForward(term);
          if (!found) {
            found = st.tenum->seekForward(term);  // place breakpoint here to debug
          }
          ASSERT_TRUE(found) << " seekForward present tnum=" << tnum
                             << " curOrd=" << curOrd << " term='" << term << "'";
          ASSERT_EQ(st.tenum->ord(), tnum) << " seekForward present tnum=" << tnum;
          curOrd = tnum;
        } else {
          // forward seek to an ABSENT term strictly between term[tnum] and
          // term[tnum+1] - the applyDeletes case (a sorted delete id missing
          // from this segment, sought via seekForward mid-iteration).
          st.makeTerm(tnum, term);
          term.push_back('!');  // sorts after term[tnum], before term[tnum+1]
          bool found = st.tenum->seekForward(term);
          if (found) {
            found = st.tenum->seekForward(term);  // place breakpoint here to debug
          }
          ASSERT_FALSE(found) << " seekForward absent between " << tnum << " and "
                              << (tnum+1) << " curOrd=" << curOrd << " term='" << term << "'";
          curOrd = tnum + 1;  // enum advances past the missed target
        }
        continue;
      }

      // seekCeil path: positions on the smallest term >= target.  Unlike
      // seekForward it may move backward, so it runs from any current position.
      if (rng.rint(100) < 40) {
        if (rng.rbool()) {
          // The ceil of a present term is that term itself.
          st.makeTerm(tnum, term);
          bool found = st.tenum->seekCeil(term);
          ASSERT_TRUE(found) << " seekCeil present tnum=" << tnum << " term='" << term << "'";
          ASSERT_EQ(st.tenum->ord(), tnum) << " seekCeil present tnum=" << tnum;
          curOrd = tnum;
        } else {
          // The ceil of a term strictly between term[tnum] and term[tnum+1] is
          // term[tnum+1] - or exhausted when tnum is the last term.
          st.makeTerm(tnum, term);
          term.push_back('!');  // sorts after term[tnum], before term[tnum+1]
          bool found = st.tenum->seekCeil(term);
          if (tnum + 1 < nTerms) {
            ASSERT_TRUE(found) << " seekCeil between tnum=" << tnum << " term='" << term << "'";
            ASSERT_EQ(st.tenum->ord(), tnum + 1) << " seekCeil ceil tnum=" << tnum;
            curOrd = tnum + 1;
          } else {
            ASSERT_FALSE(found) << " seekCeil past end tnum=" << tnum << " term='" << term << "'";
            curOrd = nTerms;  // exhausted -> position unknown, disable forward
          }
        }
        continue;
      }

      // Full seek / seekOrd path (may move backward).
      st.makeTerm(tnum, term);
      bool shouldFind = rng.rbool();
      if (!shouldFind) {
        term.push_back('!');
        if (rng.rbool()) {  // sometimes change first char to test before / after lookup failures
          term[0] = (char) rng.rbyte();
        }
      }
      bool found;
      if (shouldFind && rng.rint(100)<25) {  // look up by ord some of the time
        st.tenum->seekOrd(tnum);
        found = true;
        curOrd = tnum;
      } else {
        found = st.tenum->seek(term);
        if (found != shouldFind) {
          found = st.tenum->seek(term);  // place breakpoint here to debug
        }
        ASSERT_EQ(shouldFind, found) << " tnum=" << tnum << " term='" << term << "'";
        curOrd = found ? tnum : nTerms;  // unknown position after a miss -> disable forward
      }
      if (found) {
//        ASSERT_EQ(st.tenum->term(), term);
        auto ord = st.tenum->ord();
        ASSERT_EQ(ord, tnum) << " tnum=" << tnum << " term='" << term << "'";
        // TODO: verify the postings for the term are correct
      }
    }
  }

};

TEST_F(PostingsReaderTest, termSeek) {
  // testTermSeek(0);  // TODO: need to handle case of no terms in field in postings writer
  testTermSeek(1);
  testTermSeek(Postings::TERMS_BLOCK_SIZE-1);
  testTermSeek(Postings::TERMS_BLOCK_SIZE);
  testTermSeek(Postings::TERMS_BLOCK_SIZE+1);
  testTermSeek(Postings::TERMS_BLOCK_SIZE*2-1);
  testTermSeek(Postings::TERMS_BLOCK_SIZE*2);
  testTermSeek(Postings::TERMS_BLOCK_SIZE*2+1);
  for (int i=0; i<10; i++) {
    testTermSeek(rng.rint(1,Postings::TERMS_BLOCK_SIZE*10));
  }
}



class FieldMetadataTest : public LuxirTest {
protected:
  static std::shared_ptr<PostingsReader> openFields(test::CollectionHelper& helper, int count) {
    test::Doc doc{{"id", std::string("one")}};
    for (int i = 0; i < count; i++) {
      doc.push_back({std::format("f{:05}_i", i), (int64_t)i});
    }
    auto result = helper.index(doc, UpdateMessage::COMMIT);
    EXPECT_TRUE(result.success) << result.error_message;
    auto writer = helper.getIndexWriter();
    auto reader = writer->snapshots.readers.getReader();
    EXPECT_EQ(1u, reader->segments().size());
    return PostingsReader::create(writer->dir, reader->segments()[0].segInfo.seg_id);
  }
};

TEST_F(FieldMetadataTest, firstUseRace) {
  test::CollectionHelper helper;
  auto reader = openFields(helper, 130);
  constexpr int THREADS = 32;
  std::barrier start(THREADS);
  std::array<const PostingsReader::Field*, THREADS> infos{};
  std::array<const NumColumnFormat::LeafMeta*, THREADS> leaves{};
  std::vector<std::jthread> threads;
  for (int i = 0; i < THREADS; i++) {
    threads.emplace_back([&, i] {
      start.arrive_and_wait();
      infos[i] = reader->fieldInfo("f00064_i");
      leaves[i] = infos[i]->numericLeafMeta();
    });
  }
  threads.clear();
  ASSERT_NE(nullptr, infos[0]);
  ASSERT_NE(nullptr, leaves[0]);
  for (int i = 0; i < THREADS; i++) {
    EXPECT_EQ(infos[0], infos[i]);
    EXPECT_EQ(leaves[0], leaves[i]);
    EXPECT_EQ("f00064_i", (std::string_view)infos[i]->fieldname);
    EXPECT_EQ(FieldType::INT, infos[i]->type);
    EXPECT_EQ(1, infos[i]->docsWithField);
    EXPECT_EQ(1, infos[i]->numValues);
    ASSERT_TRUE(infos[i]->columnBounds);
    EXPECT_EQ(64, infos[i]->columnBounds->min);
    EXPECT_EQ(64, infos[i]->columnBounds->max);
  }
  auto stats = reader->fieldCacheStats();
  EXPECT_EQ(1u, stats.pages);
  EXPECT_EQ(1u, stats.entries);
  RecordProperty("field_entry_bytes", (int)sizeof(PostingsReader::Field));
}

TEST_F(FieldMetadataTest, columnBoundsIncludeSparseAndMultipleValues) {
  test::CollectionHelper helper;
  helper.index(test::flatdoc("id", "one", "dense_i", INT64_MIN + 5,
      "sparse_i", -4, "multi_is", std::vector<int64_t>{-100, 300}), UpdateMessage::NO_COMMIT);
  helper.index(test::flatdoc("id", "two", "dense_i", 2,
      "multi_is", std::vector<int64_t>{4, 4}), UpdateMessage::NO_COMMIT);
  ASSERT_TRUE(helper.index(test::flatdoc("id", "three", "dense_i", INT64_MAX - 7,
      "sparse_i", 9), UpdateMessage::COMMIT).success);
  auto reader = helper.getIndexWriter()->snapshots.readers.getReader();
  auto& postings = reader->segments()[0].postingsReader();
  ASSERT_FALSE(postings.fieldInfo("id")->columnBounds);
  for (auto [name, low, high] : {std::tuple{"dense_i", INT64_MIN + 5, INT64_MAX - 7},
                                {"sparse_i", (int64_t)-4, (int64_t)9},
                                {"multi_is", (int64_t)-100, (int64_t)300}}) {
    auto* field = postings.fieldInfo(name);
    ASSERT_NE(nullptr, field);
    ASSERT_TRUE(field->columnBounds);
    EXPECT_EQ(low, field->columnBounds->min);
    EXPECT_EQ(high, field->columnBounds->max);
  }
}

TEST_F(FieldMetadataTest, ordinalsAndPageBoundaries) {
  test::CollectionHelper helper;
  auto reader = openFields(helper, 130);
  EXPECT_EQ(0u, reader->fieldCacheStats().pages);
  EXPECT_EQ(nullptr, reader->fieldInfo(""));
  EXPECT_EQ(nullptr, reader->fieldInfo("f00064_missing"));
  EXPECT_EQ(nullptr, reader->fieldInfo("zzzz"));
  EXPECT_EQ(nullptr, reader->fieldInfo(-1));
  EXPECT_EQ(nullptr, reader->fieldInfo(reader->numFields()));
  EXPECT_EQ(0u, reader->fieldCacheStats().pages);
  FieldReader fields(*reader);
  int32_t current = -1;
  for (int32_t ordinal : {0, 63, 64, 65, reader->numFields() - 1}) {
    while (current < ordinal) {
      ASSERT_TRUE(fields.readNextField());
      current++;
    }
    SegFieldInfo expected{};
    fields.readFieldInfo(expected);
    auto* info = reader->fieldInfo(ordinal);
    ASSERT_NE(nullptr, info);
    EXPECT_EQ(info, reader->fieldInfo((std::string_view)expected.fieldname));
    EXPECT_EQ((std::string_view)expected.fieldname, (std::string_view)info->fieldname);
    EXPECT_EQ(expected.type, info->type);
    EXPECT_EQ(expected.flags, info->flags);
    EXPECT_EQ(expected.docsWithField, info->docsWithField);
    EXPECT_EQ(expected.numValues, info->numValues);
    EXPECT_EQ(expected.columnLoc.filenum(), info->columnLoc.filenum());
    EXPECT_EQ(expected.columnLoc.offset(), info->columnLoc.offset());
  }
  EXPECT_EQ(3u, reader->fieldCacheStats().pages);
}

TEST_F(FieldMetadataTest, untouchedFieldsStayUnallocated) {
  test::CollectionHelper helper;
  auto reader = openFields(helper, 4096);
  ASSERT_GT(reader->numFields(), 4096);
  EXPECT_EQ(0u, reader->fieldCacheStats().pages);
  ASSERT_NE(nullptr, reader->fieldInfo(64));
  ASSERT_NE(nullptr, reader->fieldInfo(65));
  EXPECT_EQ(1u, reader->fieldCacheStats().pages);
  ASSERT_NE(nullptr, reader->fieldInfo(4095));
  EXPECT_EQ(2u, reader->fieldCacheStats().pages);
  // Streaming discovery must not populate the untouched pages.
  FieldReader fields(*reader);
  int count = 0;
  while (fields.readNextField()) {
    SegFieldInfo info{};
    fields.readFieldInfo(info);
    count++;
  }
  EXPECT_EQ(reader->numFields(), count);
  EXPECT_EQ(2u, reader->fieldCacheStats().pages);
}

TEST_F(FieldMetadataTest, missingTermKeepsDictionaryAlive) {
  test::CollectionHelper helper;
  ASSERT_TRUE(helper.index(test::flatdoc("id", "one", "name_s", "present"),
                           UpdateMessage::COMMIT).success);
  auto reader = helper.getIndexWriter()->snapshots.readers.getReader();
  MemPool pool;
  Query::Context context(pool, *reader);
  auto* field = context.getCachedFieldInfo("name_s");
  ASSERT_NE(nullptr, field);
  EXPECT_EQ(nullptr, context.getCachedTerminfo(*field, "missing"));
  pool.make_span_zeroed<std::byte>(16384);
  auto* term = context.getCachedTerminfo(*field, "present");
  ASSERT_NE(nullptr, term);
  EXPECT_EQ(1, term->termStats.docFreq);
}

TEST_F(FieldMetadataTest, coreNamesAreSharedAndLazy) {
  test::CollectionHelper helper;
  auto fresh = openFields(helper, 130);
  auto reader = helper.getIndexWriter()->snapshots.readers.getReader();
  auto& postings = reader->segments()[0].postingsReader();
  auto before = postings.fieldCacheStats();
  auto field = reader->field("f00064_i");
  EXPECT_EQ(field, reader->field("f00064_i"));
  EXPECT_TRUE(field.exists());
  EXPECT_EQ(before.entries, postings.fieldCacheStats().entries);
  constexpr int THREADS = 16;
  std::barrier start(THREADS);
  std::array<IndexReader::FieldSegments, THREADS> views{};
  std::array<const PostingsReader::Field*, THREADS> infos{};
  std::array<const Similarity::FieldStats*, THREADS> stats{};
  std::vector<std::jthread> threads;
  for (int i = 0; i < THREADS; i++) {
    threads.emplace_back([&, i] {
      start.arrive_and_wait();
      views[i] = reader->field("f00065_i");
      infos[i] = views[i].at(0);
      stats[i] = &views[i].stats();
    });
  }
  threads.clear();
  ASSERT_NE(nullptr, infos[0]);
  for (int i = 0; i < THREADS; i++) {
    EXPECT_EQ(views[0], views[i]);
    EXPECT_EQ(stats[0], stats[i]);
    EXPECT_EQ(infos[0], infos[i]);
  }
  EXPECT_EQ(reader->maxDoc(), stats[0]->maxDoc);
  EXPECT_EQ(before.entries + 1, postings.fieldCacheStats().entries);
  auto absent = reader->field("absent_i");
  EXPECT_EQ(IndexReader::FieldSegments{}, absent);
  EXPECT_FALSE(absent.exists());
  EXPECT_EQ(nullptr, absent.at(0));
  EXPECT_EQ(absent, reader->field("absent_i"));
  MemPool pool;
  Query::Context context(pool, *reader);
  auto* cached = context.getCachedFieldInfo("f00064_i");
  ASSERT_NE(nullptr, cached);
  EXPECT_EQ(field, cached->segInfos);
  EXPECT_EQ(nullptr, context.getCachedFieldInfo("absent_i"));
  EXPECT_EQ(nullptr, context.getCachedFieldInfo("absent_i"));
  EXPECT_EQ(0u, fresh->fieldCacheStats().pages);
}

TEST_F(FieldMetadataTest, namesStayFoundWhileTheTableGrows) {
  test::CollectionHelper helper;
  constexpr int FIELDS = 130;
  openFields(helper, FIELDS);
  auto reader = helper.getIndexWriter()->snapshots.readers.getReader();
  constexpr int THREADS = 16;
  std::barrier start(THREADS);
  std::vector<std::vector<IndexReader::FieldSegments>> views(THREADS);
  std::vector<std::jthread> threads;
  for (int t = 0; t < THREADS; t++) {
    threads.emplace_back([&, t] {
      views[t].resize(FIELDS);
      start.arrive_and_wait();
      // Each thread starts elsewhere, so inserts and growth race with lookups.
      for (int round = 0; round < 2; round++) {
        for (int j = 0; j < FIELDS; j++) {
          int i = (j + t * 37) % FIELDS;
          auto view = reader->field(std::format("f{:05}_i", i));
          if (round) { EXPECT_EQ(views[t][i], view); }
          views[t][i] = view;
        }
      }
    });
  }
  threads.clear();
  for (int i = 0; i < FIELDS; i++) {
    ASSERT_TRUE(views[0][i].exists());
    EXPECT_EQ(std::format("f{:05}_i", i), views[0][i].name());
    for (int t = 1; t < THREADS; t++) EXPECT_EQ(views[0][i], views[t][i]);
  }
}

} // end namespace