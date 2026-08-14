#include <set>
#include "luxir/index/PostingsWriter.h"
#include "luxir/reader/DocsEnum.h"
#include "luxir/reader/PosEnum.h"
#include "gtest/gtest.h"
#include "test/LuxirTest.h"
#include<boost/container/static_vector.hpp>

#include "luxir/index/IntColWriter.h"

namespace luxir {

class PostingsTest : public LuxirTest {
protected:
  RAMDir dir;
  MemPool pool;
  MemPool::save_point save = pool.getSavePoint();
  std::unique_ptr<PostingsWriter> postingsWriter;
  std::unique_ptr<TextWriter> writer;
  std::string field;
  std::string term;


  // set these limits lower for easier debugging
  uint32_t positionsPerDocMax = 10;
  uint32_t docsPerTermMax = 10;
  uint32_t termsPerFieldMax = 100;


  std::unique_ptr<PostingsReader> reader;
  std::unique_ptr<FieldReader> fieldReader;
  std::unique_ptr<TermsEnum> tenum;
  std::unique_ptr<DocsPosEnum> docsEnum;
  std::unique_ptr<PosEnum> posEnum;

  Rng rng_start;
  Rng r2;


  PostingsTest() {
  }

  void initWriter() {
    pool.rewind(save);
    dir = RAMDir();  // remove all files?
    postingsWriter = std::make_unique<PostingsWriter>(dir, 0, 0x7fffffff);  // use maximum value for numDocs... nothing (currently) in text field depends on it.
    // writer = std::make_unique<TextWriter>(*postingsWriter);

    // save the RNG state
    rng_start = rng;
    // re-init secondary rng off of first
    r2.init(rng());
  }

  void initReader() {
    postingsWriter->finish();

    reader = std::make_unique<PostingsReader>(dir, 0);
    fieldReader = std::make_unique<FieldReader>(*reader);

    // restore the RNG state
    rng = rng_start;
    // re-init secondary rng off of first
    r2.init(rng());
  }

  uint32_t getPositionDelta(int nPositions) {
    // TODO: do a better job at testing boundaries
    return rng.rint(1, (INT_MAX - 1) / nPositions);
  }

  uint32_t getDocDelta(int nDocs) {
    // TODO: do a better job at testing boundaries
    return rng.rint(1, (INT_MAX - 1) / nDocs);
  }

  uint32_t getNumPositions(uint32_t numDocs) {
    unused(numDocs);
    return rng.rint(1u, positionsPerDocMax);
  }

  uint32_t getNumDocs(uint32_t numTerms) {
    unused(numTerms);
    return rng.rint(1u, docsPerTermMax);
  }

  uint32_t getNumTerms(uint32_t numFields) {
    unused(numFields);
    return rng.rint(1u, termsPerFieldMax);
  }


  // numPositions is changed to the actual number indexed (random positions can overflow max)
  void addDoc(bool read, int docid, uint32_t &numPositions) {
    uint32_t readTf = 0;
    uint32_t maxRead = numPositions;
    if (read) {
      auto readid = docsEnum->nextDoc();
      ASSERT_EQ(docid, readid);
      readTf = docsEnum->termFreq();  // read the tf first, but don't compare it until later
      if (r2.rbool()) {
        // don't read all of the positions
        maxRead = 0;  // for now, don't read any (simplate skipping)
      }
      if (maxRead > 0 || r2.rbool()) { // sometimes call startPositions even if we aren't going to read positions
        posEnum->startPositions();
      }
    } else {
      writer->startDoc(docid);
    }
    uint64_t position = -1;
    uint32_t actualPositions = 0;
    for (uint32_t i = 0; i < numPositions; i++) {   // TODO: introduce constants for limits
      auto delta = getPositionDelta(numPositions);
      position += delta;
      if (position >= INT_MAX) {
        break;
      }
      actualPositions++;
      if (read) {
        if (maxRead > 0) {
          maxRead--;
          auto pos = posEnum->nextPosition();
          // std::cout << "\t\tread pos=" << pos << std::endl;
          ASSERT_EQ(position, pos);
        }
      } else {
        // std::cout << "\t\tadding posDelta=" << delta << " pos=" << position << std::endl;
        writer->addPositionDelta(delta);
      }
    }
    if (read) {
      ASSERT_EQ(readTf, actualPositions);
      // undefined behavior reading positions past termFreq
    } else {
      writer->endDoc(docid);
    }
    numPositions = actualPositions;
  }


  void addTerm(bool read, const std::string &term, uint32_t numDocs) {
    TermRef termRef;
    termRef.init(nullptr,0);  // just to get rid of "possibly uninitialized" warning

    uint32_t numDocsRead = 0;
    if (read) {
      if (numDocs > 0) {
        ASSERT_TRUE(tenum->nextTerm());
        ASSERT_EQ(tenum->term(), term);
        posEnum.reset();
        docsEnum = std::make_unique<DocsPosEnum>(*tenum);
        posEnum = std::make_unique<PosEnum>(*docsEnum);
        numDocsRead = docsEnum->numDocs();
      }
    } else {
      termRef = TermRef(pool, term.data(), term.size());
      writer->startTerm(termRef);
    }
    uint64_t docid = 0;
    int actualDocs = 0;
    uint64_t actualttf = 0;
    for (uint32_t i = 0; i < numDocs; i++) {
      auto docDelta = getDocDelta(numDocs);
      docid += docDelta;
      if (docid > INT_MAX) {
        break;
      }
      actualDocs++;
      uint32_t numPositions = getNumPositions(numDocs);
      addDoc(read, (int) docid, numPositions);
      actualttf += numPositions;
    }
    if (read) {
      if (numDocs > 0) {
        ASSERT_EQ(actualDocs, numDocsRead);
        ASSERT_EQ(actualttf, docsEnum->totalTermFreq());
      }
    } else {
      writer->endTerm(termRef);
    }
  }

  SegFieldInfo fieldInfo;
  void addField(bool read, const std::string &fname, uint32_t numTerms) {
    std::string term = "term";
    term.resize(12);

    if (read) {
      ASSERT_TRUE(fieldReader->readNextField());
      ASSERT_EQ(fname, fieldReader->name());
      fieldReader->readFieldInfo(fieldInfo);
      tenum = std::make_unique<TermsEnum>(pool, *reader, fieldInfo);
    } else {
      writer = std::make_unique<TextWriter>(*postingsWriter);
      writer->startField(fname);
    }
    int realNumTerms = 0;
    for (uint32_t i = 0; i < numTerms; i++) {
      // std::format not implemented yet...
      sprintf(term.data() + 4, "%08d", i);
      auto ndocs = getNumDocs(numTerms);
      if (ndocs > 0) ++realNumTerms;  // if number of docs for term ends up being 0, we should drop the term.
      addTerm(read, term, ndocs);
    }
    if (read) {
      ASSERT_EQ(tenum->numTerms(), realNumTerms);
    } else {
      writer->endField();
      writer.reset();
    }
  }

  void addFields(bool read, uint32_t numFields) {
    std::string fname = "field";
    fname.resize(13);

    for (uint32_t i = 0; i < numFields; i++) {
      // std::format not implemented yet...
      sprintf(fname.data() + 5, "%08d", i);
      addField(read, fname, getNumTerms(numFields));
    }
  }

  int stackfill(uint64_t fill, int sz) {
    Rng r;
    uint64_t *p = (uint64_t *) alloca(sz * sizeof(uint64_t));
    uint64_t ret = r();
    for (int i = 0; i < sz; i++) {
      p[i] = r();
    }
    // conspire to set all the mem to the same thing without the compiler optimizing it away
    for (int i = 0; i < sz; i++) {
      uint64_t otherIdx = r() % sz;
      if (otherIdx != 7) {
        p[i] = fill;
      }
      ret += p[otherIdx];
    }
    return ret;
  }

};


TEST_F(PostingsTest, basic) {
  RAMDir dir;
  MemPool pool;
  PostingsWriter postingsWriter(dir, 0, 100);
  std::string t1 = "term1";
  std::string t2 = "term2";
  std::string ta = "termA";
  TermRef term1(pool, t1.data(), t1.size());
  TermRef term2(pool, t2.data(), t2.size());
  TermRef terma(pool, ta.data(), ta.size());

  {
    TextWriter writer(postingsWriter);

    writer.startField("field1");
    writer.startTerm(term1);  // single doc, single position... this should be pulsed
    writer.startDoc(44);
    writer.addPositionDelta(1);
    writer.endDoc(44);
    writer.startDoc(55);
    writer.addPositionDelta(555);
    writer.addPositionDelta(111);
    writer.endDoc(55);
    writer.startDoc(56);
    writer.addPositionDelta(7);
    writer.addPositionDelta(2);
    writer.endDoc(56);
    writer.endTerm(term1);

    writer.startTerm(term2);
    writer.startDoc(7);
    writer.addPositionDelta(5);
    writer.addPositionDelta(3);
    writer.addPositionDelta(10);
    writer.endDoc(7);
    writer.startDoc(22);
    writer.addPositionDelta(0);
    writer.addPositionDelta(300);
    writer.endDoc(22);
    writer.endTerm(term2);

    writer.endField();
  }
  {
    TextWriter writer(postingsWriter);

    writer.startField("field2");
    writer.startTerm(terma);
    writer.startDoc(0);
    writer.addPositionDelta(3);
    writer.addPositionDelta(1);
    writer.endDoc(0);
    writer.endTerm(terma);
    writer.endField();
  }

  postingsWriter.finish();


  PostingsReader reader(dir, 0);

  FieldReader fieldReader(reader);
  while (fieldReader.readNextField()) {
    LOG_TRACE("FIELD NAME name={}", fieldReader.name());
    SegFieldInfo fieldInfo;
    fieldReader.readFieldInfo(fieldInfo);
    TermsEnum tenum(pool, reader, fieldInfo);
    while (tenum.nextTerm()) {
      LOG_TRACE("\tTERM={} ord={}", tenum.term(), tenum.ord());
      // if (tenum.ord()==0) continue; // skip first term, good for figuring out of second term errors are due to reader or writer.

      DocsPosEnum docsEnum(tenum);
      PosEnum posEnum(docsEnum);
      auto ndocs = docsEnum.numDocs();
      LOG_TRACE("\t\tnumDocs={} totalTermFreq={}" , docsEnum.numDocs(), docsEnum.totalTermFreq());

      for (int i = 0; i < ndocs; i++) {
        auto id = docsEnum.nextDoc();
        auto tfreq = docsEnum.termFreq();
        LOG_TRACE("\t\t\tdocid={} termFreq={}", id, tfreq);
        unused(id); unused(tfreq);
        if (i==1) { continue; }  // test skipping reading a docs positions
        posEnum.startPositions();
        for (int j = 0; j < tfreq; j++) {
          auto pos = posEnum.nextPosition();
          LOG_TRACE("\t\t\t\tpos={}",pos);
          unused(pos);
        }
      }
    }
  }

}

// Exercise the index-options ladder: DOCS (no freqs/positions),
// DOCS_AND_FREQS (freqs, no positions), and full positions.  Each level must
// round-trip through the pulsed path (single doc), the StreamVByte tail, and a
// full doc block.  tf is only stored when freqs are indexed; otherwise the
// reader must report 1.  Positions are only present at the top level.
TEST_F(PostingsTest, levelLadder) {
  struct Doc { int id; int tf; };
  struct Term { std::string name; std::vector<Doc> docs; };

  for (int level = 0; level < 3; level++) {
    FieldType::flag_type flags =
        level == 0 ? FieldType::INDEX_DOCS :
        level == 1 ? FieldType::INDEX_DOCS_FREQS :
                     FieldType::INDEX_DOCS_FREQS_POSITIONS;
    bool hasFreqs = FieldType::hasFreqs(flags);
    bool hasPositions = FieldType::hasPositions(flags);

    // terms must be written in sorted order: block < pulse < tail.
    std::vector<Term> terms;
    Term blk{"block", {}};
    int nBlockDocs = Postings::DOCS_BLOCK_SIZE + 5;  // forces a block + tail
    for (int d = 0; d < nBlockDocs; d++) blk.docs.push_back({d, (d % 3) + 1});
    terms.push_back(std::move(blk));
    terms.push_back({"pulse", {{3, 1}}});  // single doc, single occurrence -> pulsed
    Term tail{"tail", {}};
    for (int d = 0; d < 5; d++) tail.docs.push_back({d * 7 + 1, d + 1});
    terms.push_back(std::move(tail));

    RAMDir dir;
    MemPool pool;
    PostingsWriter postingsWriter(dir, 0, Postings::DOCS_BLOCK_SIZE + 20);
    {
      TextWriter writer(postingsWriter);
      auto& finfo = postingsWriter.addField("f");
      finfo.type = FieldType::TEXT;
      finfo.flags = flags;
      writer.startField(&finfo);
      for (auto& t : terms) {
        TermRef tref(pool, t.name.data(), t.name.size());
        writer.startTerm(tref);
        for (auto& d : t.docs) {
          // Positions stream in via startDoc/addPositionDelta/endDoc; positionless
          // fields state the freq directly via addDoc.  Writing the actual occurrence
          // count at every level (including DOCS-only, where freqs are not stored)
          // verifies that a non-freq field with multiple occurrences in one doc still
          // reports ttf==docfreq, and that field-level sumTotalTermFreq matches the
          // per-term ttf the reader sees.
          if (hasPositions) {
            writer.startDoc(d.id);
            for (int o = 0; o < d.tf; o++) writer.addPositionDelta(o == 0 ? 1 : 2);
            writer.endDoc(d.id);
          } else {
            writer.addDoc(d.id, d.tf);
          }
        }
        writer.endTerm(tref);
      }
      writer.endField();
    }
    postingsWriter.finish();

    PostingsReader reader(dir, 0);
    FieldReader fieldReader(reader);
    ASSERT_TRUE(fieldReader.readNextField());
    SegFieldInfo fieldInfo;
    fieldReader.readFieldInfo(fieldInfo);
    ASSERT_EQ(FieldType::hasFreqs(fieldInfo.flags), hasFreqs) << "level " << level;
    ASSERT_EQ(FieldType::hasPositions(fieldInfo.flags), hasPositions) << "level " << level;

    // Field-level stats must agree with the per-term ttf the reader reports: ttf is
    // sum-of-freqs when freqs are indexed, else docfreq (each doc counts once).
    int64_t expectedSumDf = 0, expectedSumTtf = 0;
    for (auto& t : terms) {
      expectedSumDf += (int64_t) t.docs.size();
      for (auto& d : t.docs) expectedSumTtf += hasFreqs ? d.tf : 1;
    }
    ASSERT_EQ(fieldInfo.sumDocFreq, expectedSumDf) << "level " << level;
    ASSERT_EQ(fieldInfo.sumTotalTermFreq, expectedSumTtf) << "level " << level;

    auto check = [&]<DocsEnumTier Tier>() {
      TermsEnum tenum(pool, reader, fieldInfo);
      for (auto& t : terms) {
        ASSERT_TRUE(tenum.nextTerm()) << "level " << level << " term " << t.name;
        ASSERT_EQ(tenum.term(), t.name);
        BasicDocsEnum<Tier> de(tenum);
        std::unique_ptr<PosEnum> posEnum;
        if constexpr (Tier == DocsEnumTier::POSITIONS) {
          posEnum = std::make_unique<PosEnum>(de);
        }
        ASSERT_EQ(de.numDocs(), (int)t.docs.size()) << "level " << level << " term " << t.name;

        int64_t expectedTtf = 0;
        for (auto& d : t.docs) expectedTtf += hasFreqs ? d.tf : 1;
        ASSERT_EQ(de.totalTermFreq(), expectedTtf) << "level " << level << " term " << t.name;

        for (auto& d : t.docs) {
          ASSERT_EQ(de.nextDoc(), d.id) << "level " << level << " term " << t.name;
          int expectedTf = hasFreqs ? d.tf : 1;
          ASSERT_EQ(de.termFreq(), expectedTf)
              << "level " << level << " term " << t.name << " doc " << d.id;
          if constexpr (Tier == DocsEnumTier::POSITIONS) {
            // The written deltas are 1, then 2,2,...; because positions reset per
            // doc, occurrence o lands at 2*o. Reading past tf positions is outside
            // the codec contract.
            posEnum->startPositions();
            for (int o = 0; o < expectedTf; o++) {
              ASSERT_EQ(posEnum->nextPosition(), 2 * o)
                  << "level " << level << " term " << t.name << " doc " << d.id << " occ " << o;
            }
          }
        }
        ASSERT_EQ(de.nextDoc(), INT_MAX) << "level " << level << " term " << t.name;
      }
      ASSERT_FALSE(tenum.nextTerm());
    };
    if (hasPositions) {
      check.template operator()<DocsEnumTier::POSITIONS>();
    } else {
      check.template operator()<DocsEnumTier::FREQS>();
    }
  }
}

// Verify the file over-read padding contract used by PostingsWriter::finish().
// Any codec's output can be the last bytes of a data file, so a decoder that reads
// past the encoded bytes can run past the exact-sized store buffer or mmap. Decode
// each codec from a heap allocation sized to exactly encodedSize plus its documented
// over-read; under ASan the redzone immediately after the allocation catches reads
// beyond that bound. Numeric and PFOR decode from the exact byte range and need no
// slack. The old IntColWriter "31 bytes" note referred to the removed
// SIMDCompressionAndIntersection codec. The StreamVByte AVX tail must stay within
// SVB_OVERREAD_PAD.
TEST_F(PostingsTest, codecFileOverreadBounds) {
  // LuxirSIMDFor numeric column codec: claim exact (decode from a 0-slack buffer).
  for (uint8_t bits : {(uint8_t)1, (uint8_t)7, (uint8_t)17, (uint8_t)31, (uint8_t)32}) {
    uint32_t mask = bits == 32 ? ~0u : ((1u << bits) - 1);
    for (uint32_t n : {1u, 5u, 31u, 100u, 128u, 200u, 333u}) {
      std::vector<uint32_t> vals(n);
      for (auto& v : vals) v = (uint32_t) rng() & mask;
      std::vector<char> work(n * sizeof(uint32_t) + 64);
      uint32_t encSz = work.size();
      IndexCodec::numericCodec.encodeWithMeta(vals.data(), n, work.data(), encSz, 0, bits);
      char* exact = new char[encSz];  // ASan redzone right after -> over-read faults
      memcpy(exact, work.data(), encSz);
      std::vector<uint32_t> out(n);
      IndexCodec::numericCodec.decodeWithMeta(exact, out.data(), n, bits);
      delete[] exact;
      ASSERT_EQ(out, vals) << "numeric bits=" << (int)bits << " n=" << n;
    }
  }

  // LuxirPFORd docs + LuxirPFOR positions codecs: full-block decodeBlock, claim exact.
  auto checkBlockCodec = [&](U32Codec& codec, uint32_t blockSize, const char* name) {
    std::vector<uint32_t> vals(blockSize);
    uint32_t d = 0;
    for (auto& v : vals) { d += rng.rint(1, 1000); v = d; }  // monotonic (delta-coded)
    std::vector<uint32_t> expected = vals;  // LuxirPFORd delta-codes its input in place
    std::vector<char> work(blockSize * sizeof(uint32_t) + 1024);
    uint32_t encSz = work.size();
    codec.encodeBlock(vals.data(), blockSize, work.data(), encSz);
    char* exact = new char[encSz];
    memcpy(exact, work.data(), encSz);
    std::vector<uint32_t> out(blockSize);
    uint32_t outSz = blockSize;
    codec.decodeBlock(exact, encSz, out.data(), outSz);
    delete[] exact;
    ASSERT_EQ(outSz, blockSize) << name;
    ASSERT_EQ(out, expected) << name;
  };
  checkBlockCodec(IndexCodec::docCodec, Postings::DOCS_BLOCK_SIZE, "docCodec");
  checkBlockCodec(IndexCodec::posCodec, Postings::POSITIONS_BLOCK_SIZE, "posCodec");

  // StreamVByte AVX tail (docs d1 + freqs plain): claim over-read <= SVB_OVERREAD_PAD.
  // Buffer is sized to exactly the encoded length + SVB_OVERREAD_PAD; a larger over-read
  // would fault.  n covers the scalar path (<32) and the AVX path (>=32, multiples and not).
  for (uint32_t n : {1u, 5u, 32u, 33u, 64u, 96u, 127u}) {
    std::vector<uint32_t> docs(n), freqs(n);
    uint32_t d = 0;
    for (auto& v : docs) { d += rng.rint(1, 100000); v = d; }
    constexpr uint32_t widthCases[] = {1u, 0x100u, 0x10000u, 0x1000000u};
    for (uint32_t i = 0; i < n; i++) freqs[i] = widthCases[i & 3];
    uint32_t kb = svbKeyBytes(n);
    std::vector<uint8_t> dkeys(kb), ddata(n * 4 + 32), tkeys(kb), tdata(n * 4 + 32);
    uint8_t* ddEnd = svb_encode_scalar_d1_init(docs.data(), dkeys.data(), ddata.data(), n, 0);
    uint8_t* tdEnd = svb_encode_scalar(freqs.data(), tkeys.data(), tdata.data(), n);
    uint32_t dDataSz = (uint32_t)(ddEnd - ddata.data());
    uint32_t tDataSz = (uint32_t)(tdEnd - tdata.data());
    uint32_t encSz = kb + dDataSz + kb + tDataSz;  // [dkeys][ddata][tkeys][tdata]
    char* buf = new char[encSz + SVB_OVERREAD_PAD];
    uint8_t* p = (uint8_t*) buf;
    memcpy(p, dkeys.data(), kb);
    memcpy(p + kb, ddata.data(), dDataSz);
    memcpy(p + kb + dDataSz, tkeys.data(), kb);
    memcpy(p + kb + dDataSz + kb, tdata.data(), tDataSz);
    std::vector<uint32_t> outDocs(n + 8), outFreqs(n + 8);
    uint8_t* de = svb_decode_avx_d1_init(outDocs.data(), p, p + kb, n, 0);
    uint8_t* te = svb_decode_avx_simple(outFreqs.data(), de, de + kb, n);
    ASSERT_EQ(svbEncodedBytes(p, n), kb + dDataSz);
    ASSERT_EQ(svbEncodedBytes(de, n), kb + tDataSz);
    ASSERT_EQ(te - de, svbEncodedBytes(de, n));
    delete[] buf;
    for (uint32_t i = 0; i < n; i++) {
      ASSERT_EQ(outDocs[i], docs[i]) << "svb docs n=" << n << " i=" << i;
      ASSERT_EQ(outFreqs[i], freqs[i]) << "svb freqs n=" << n << " i=" << i;
    }
  }
}

TEST_F(PostingsTest, blockPositions) {
  RAMDir dir;
  MemPool pool;
  PostingsWriter postingsWriter(dir, 0, 44);
  int nPos = Postings::POSITIONS_BLOCK_SIZE * 2 + 2; // TODO: parameterize
  int delta = 2;
  int nPos2 = Postings::POSITIONS_BLOCK_SIZE * 2 + 2; // TODO: parameterize
  int delta2 = 3;
  {
    TextWriter writer(postingsWriter);
    std::string t1 = "term1";
    TermRef term1(pool, t1.data(), t1.size());

    writer.startField("field1");
    writer.startTerm(term1);
    writer.startDoc(42);

    for (int i = 0; i < nPos; i++) {
      writer.addPositionDelta(delta);
    }
    writer.endDoc(42);

    writer.startDoc(43);
    for (int i = 0; i < nPos; i++) {
      writer.addPositionDelta(delta2);
    }
    writer.endDoc(43);

    writer.endTerm(term1);
    writer.endField();
  }
  postingsWriter.finish();

  PostingsReader reader(dir, 0);

  FieldReader fieldReader(reader);
  ASSERT_TRUE(fieldReader.readNextField());
  ASSERT_EQ(fieldReader.name(), std::string_view("field1"));
  SegFieldInfo fieldInfo;
  fieldReader.readFieldInfo(fieldInfo);
  TermsEnum tenum(pool, reader, fieldInfo);
  ASSERT_EQ(tenum.numTerms(), 1);
  ASSERT_TRUE(tenum.nextTerm());
  ASSERT_EQ(tenum.ord(), 0);
  ASSERT_EQ(tenum.term(), std::string_view("term1"));

  DocsPosEnum docsEnum(tenum);
  PosEnum posEnum(docsEnum);
  ASSERT_EQ(docsEnum.numDocs(), 2);
  ASSERT_EQ(docsEnum.totalTermFreq(), nPos + nPos2);

  auto id = docsEnum.nextDoc();
  auto tfreq = docsEnum.termFreq();
  ASSERT_EQ(id, 42);
  ASSERT_EQ(tfreq, nPos);
  // if (false)  // skip reading positions
  {
    posEnum.startPositions();
    int32_t lastPos = -1;
    for (int i = 0; i < tfreq; i++) {
      auto pos = posEnum.nextPosition();
      auto posDelta = pos - lastPos;
      lastPos = pos;
      ASSERT_EQ(posDelta, delta);
    }
  }

  auto id2 = docsEnum.nextDoc();
  auto tfreq2 = docsEnum.termFreq();
  ASSERT_EQ(id2, 43);
  ASSERT_EQ(tfreq2, nPos2);
  {
    posEnum.startPositions();
    int32_t lastPos = -1;
    for (int i = 0; i < tfreq; i++) {
      auto pos = posEnum.nextPosition();
      auto posDelta = pos - lastPos;
      lastPos = pos;
      ASSERT_EQ(posDelta, delta2);
    }
  }

  ASSERT_FALSE(tenum.nextTerm());
  ASSERT_FALSE(fieldReader.readNextField());
}

TEST_F(PostingsTest, blockTerms) {
  RAMDir dir;
  MemPool pool;
  int nTerms = Postings::TERMS_BLOCK_SIZE + 1;
  PostingsWriter postingsWriter(dir, 0, nTerms);
  std::string tstr = "term";
  tstr.resize(12);
  {
    TextWriter writer(postingsWriter);
    writer.startField("field1");


    for (int i = 0; i < nTerms; i++) {
      sprintf(tstr.data() + 4, "%08d", i);
      TermRef term(pool, tstr.data(), tstr.size());
      writer.startTerm(term);
      writer.startDoc(i);
      writer.addPositionDelta(i * 2);
      writer.addPositionDelta(1);
      writer.endDoc(i);
      writer.endTerm(term);
    }
    writer.endField();
  }
  postingsWriter.finish();
  PostingsReader reader(dir, 0);

  FieldReader fieldReader(reader);
  ASSERT_TRUE(fieldReader.readNextField());
  ASSERT_EQ(fieldReader.name(), std::string_view("field1"));
  SegFieldInfo fieldInfo;
  fieldReader.readFieldInfo(fieldInfo);
  TermsEnum tenum(pool, reader, fieldInfo);
  ASSERT_EQ(tenum.numTerms(), nTerms);

  for (int i=0; i<nTerms; i++) {
    sprintf(tstr.data() + 4, "%08d", i);

    ASSERT_TRUE(tenum.nextTerm());
    ASSERT_EQ(tenum.ord(), i);
    ASSERT_EQ(tenum.term(), tstr);


    DocsPosEnum docsEnum(tenum);
    PosEnum posEnum(docsEnum);
    ASSERT_EQ(docsEnum.numDocs(), 1);
    ASSERT_EQ(docsEnum.totalTermFreq(), 2);
    ASSERT_EQ(docsEnum.nextDoc(), i);
    ASSERT_EQ(docsEnum.termFreq(), 2);

    posEnum.startPositions();
    ASSERT_EQ(posEnum.nextPosition(), i*2-1);
    ASSERT_EQ(posEnum.nextPosition(), i*2);
    ASSERT_EQ(posEnum.nextPosition(), PosEnum::END);

    ASSERT_EQ(docsEnum.nextDoc(), INT_MAX);
  }

  ASSERT_FALSE(tenum.nextTerm());
  ASSERT_FALSE(fieldReader.readNextField());
}



TEST_F(PostingsTest, seekForward) {
  RAMDir dir;
  MemPool pool;
  // Create enough terms to span multiple blocks (TERMS_BLOCK_SIZE=32)
  int nTerms = Postings::TERMS_BLOCK_SIZE * 3 + 5;  // 101 terms across 4 blocks
  PostingsWriter postingsWriter(dir, 0, nTerms);
  std::string tstr = "term";
  tstr.resize(12);

  // Generate sorted term names: term00000000, term00000001, ...
  {
    TextWriter writer(postingsWriter);
    writer.startField("field1");
    for (int i = 0; i < nTerms; i++) {
      sprintf(tstr.data() + 4, "%08d", i);
      TermRef term(pool, tstr.data(), tstr.size());
      writer.startTerm(term);
      writer.startDoc(i);
      writer.addPositionDelta(1);
      writer.endDoc(i);
      writer.endTerm(term);
    }
    writer.endField();
  }
  postingsWriter.finish();

  PostingsReader reader(dir, 0);
  FieldReader fieldReader(reader);
  ASSERT_TRUE(fieldReader.readNextField());
  SegFieldInfo fieldInfo;
  fieldReader.readFieldInfo(fieldInfo);

  auto makeTerm = [&](int i) {
    sprintf(tstr.data() + 4, "%08d", i);
    return std::string(tstr);
  };

  // Test 1: seekForward through every term sequentially (same as iterating)
  {
    TermsEnum tenum(pool, reader, fieldInfo);
    ASSERT_TRUE(tenum.seek(makeTerm(0)));
    ASSERT_EQ(tenum.term(), makeTerm(0));
    for (int i = 1; i < nTerms; i++) {
      auto t = makeTerm(i);
      ASSERT_TRUE(tenum.seekForward(t)) << "seekForward failed for term " << i;
      ASSERT_EQ(tenum.term(), t);
    }
  }

  // Test 2: seekForward skipping terms within the same block
  {
    TermsEnum tenum(pool, reader, fieldInfo);
    ASSERT_TRUE(tenum.seek(makeTerm(0)));
    // Skip by 3 within block
    for (int i = 3; i < Postings::TERMS_BLOCK_SIZE; i += 3) {
      ASSERT_TRUE(tenum.seekForward(makeTerm(i))) << "same-block skip failed for " << i;
      ASSERT_EQ(tenum.term(), makeTerm(i));
    }
  }

  // Test 3: seekForward across block boundaries
  {
    TermsEnum tenum(pool, reader, fieldInfo);
    ASSERT_TRUE(tenum.seek(makeTerm(5)));
    // Jump across blocks
    int targets[] = {35, 65, 95, nTerms - 1};
    for (int t : targets) {
      ASSERT_TRUE(tenum.seekForward(makeTerm(t))) << "cross-block failed for " << t;
      ASSERT_EQ(tenum.term(), makeTerm(t));
    }
  }

  // Test 4: seekForward for missing terms (between existing terms)
  {
    TermsEnum tenum(pool, reader, fieldInfo);
    ASSERT_TRUE(tenum.seek(makeTerm(0)));
    // "term00000000x" is between term00000000 and term00000001 lexicographically
    ASSERT_FALSE(tenum.seekForward("term00000000x"));
    // Should still be able to find a later term
    ASSERT_TRUE(tenum.seekForward(makeTerm(10)));
    ASSERT_EQ(tenum.term(), makeTerm(10));
  }

  // Test 5: seekForward for term past the end
  {
    TermsEnum tenum(pool, reader, fieldInfo);
    ASSERT_TRUE(tenum.seek(makeTerm(0)));
    ASSERT_FALSE(tenum.seekForward("zzzzz"));
  }

  // Test 6: seekForward with postings iteration interleaved (simulates applyDeletes)
  {
    TermsEnum tenum(pool, reader, fieldInfo);
    ASSERT_TRUE(tenum.seek(makeTerm(2)));
    DocsOnlyEnum docsEnum1(tenum);
    ASSERT_EQ(docsEnum1.next(), 2);

    ASSERT_TRUE(tenum.seekForward(makeTerm(4)));
    DocsOnlyEnum docsEnum2(tenum);
    ASSERT_EQ(docsEnum2.next(), 4);

    // Same block seek after postings iteration
    ASSERT_TRUE(tenum.seekForward(makeTerm(7)));
    DocsOnlyEnum docsEnum3(tenum);
    ASSERT_EQ(docsEnum3.next(), 7);

    // Cross-block seek after postings iteration
    ASSERT_TRUE(tenum.seekForward(makeTerm(40)));
    DocsOnlyEnum docsEnum4(tenum);
    ASSERT_EQ(docsEnum4.next(), 40);
  }

  // Test 7: mix of found and not-found in forward order (simulates sparse delete list)
  {
    TermsEnum tenum(pool, reader, fieldInfo);
    ASSERT_TRUE(tenum.seek(makeTerm(0)));
    // Seek for a missing term, then the next real term
    ASSERT_FALSE(tenum.seekForward("term00000003x"));
    ASSERT_TRUE(tenum.seekForward(makeTerm(5)));
    ASSERT_FALSE(tenum.seekForward("term00000005x"));
    ASSERT_TRUE(tenum.seekForward(makeTerm(33)));  // first term of second block
    ASSERT_FALSE(tenum.seekForward("term00000033x"));
    ASSERT_TRUE(tenum.seekForward(makeTerm(34)));
  }

  // Test 8: seekForward every term with postings iteration (exact applyDeletes pattern)
  {
    TermsEnum tenum(pool, reader, fieldInfo);
    bool first = true;
    for (int i = 0; i < nTerms; i++) {
      auto t = makeTerm(i);
      bool found = first ? tenum.seek(t) : tenum.seekForward(t);
      first = false;
      ASSERT_TRUE(found) << "failed at term " << i;
      ASSERT_EQ(tenum.term(), t);
      DocsOnlyEnum docsEnum(tenum);
      ASSERT_EQ(docsEnum.next(), i);
    }
  }

  // Test 8b: same but skipping every other term (with postings iteration)
  {
    TermsEnum tenum(pool, reader, fieldInfo);
    bool first = true;
    for (int i = 0; i < nTerms; i += 2) {
      auto t = makeTerm(i);
      bool found = first ? tenum.seek(t) : tenum.seekForward(t);
      first = false;
      ASSERT_TRUE(found) << "skip-2 failed at term " << i;
      ASSERT_EQ(tenum.term(), t);
      DocsOnlyEnum docsEnum(tenum);
      ASSERT_EQ(docsEnum.next(), i);
    }
  }

  // Test 9: seekForward with alternating found/not-found + postings iteration
  {
    TermsEnum tenum(pool, reader, fieldInfo);
    ASSERT_TRUE(tenum.seek(makeTerm(0)));
    DocsOnlyEnum de0(tenum);
    ASSERT_EQ(de0.next(), 0);

    for (int i = 1; i < nTerms; i++) {
      // Seek for a term that doesn't exist (appended 'x')
      auto missing = makeTerm(i - 1) + "x";
      ASSERT_FALSE(tenum.seekForward(missing)) << "should miss at " << missing;

      // Then seek for the real term
      auto t = makeTerm(i);
      ASSERT_TRUE(tenum.seekForward(t)) << "should find term " << i;
      ASSERT_EQ(tenum.term(), t);
      DocsOnlyEnum de(tenum);
      ASSERT_EQ(de.next(), i);
    }
  }
}

// Test seekForward with short numeric IDs similar to the multithreaded overwrite test.
// These have variable lengths, different prefixes, and less prefix sharing than padded terms.
TEST_F(PostingsTest, seekForwardNumericIds) {
  RAMDir dir;
  MemPool pool;

  // Generate IDs like the multithreaded test: thread*1000 + localDoc
  // e.g. 1000,1001,1002,1003, 2000,2001,..., 15000,...,15003
  std::vector<std::string> ids;
  for (int tid = 0; tid < 16; tid++) {
    for (int doc = 0; doc < 4; doc++) {
      ids.push_back(std::to_string(tid * 1000 + doc));
    }
  }
  std::sort(ids.begin(), ids.end());

  int nTerms = (int)ids.size();
  PostingsWriter postingsWriter(dir, 0, nTerms);
  {
    TextWriter writer(postingsWriter);
    writer.startField("id");
    for (int i = 0; i < nTerms; i++) {
      TermRef term(pool, ids[i].data(), ids[i].size());
      writer.startTerm(term);
      writer.startDoc(i);
      writer.addPositionDelta(1);
      writer.endDoc(i);
      writer.endTerm(term);
    }
    writer.endField();
  }
  postingsWriter.finish();

  PostingsReader reader(dir, 0);
  FieldReader fieldReader(reader);
  ASSERT_TRUE(fieldReader.readNextField());
  SegFieldInfo fieldInfo;
  fieldReader.readFieldInfo(fieldInfo);

  // Test: seek first, then seekForward every remaining term with postings iteration
  {
    TermsEnum tenum(pool, reader, fieldInfo);
    ASSERT_TRUE(tenum.seek(ids[0]));
    DocsOnlyEnum de0(tenum);
    ASSERT_EQ(de0.next(), 0);

    for (int i = 1; i < nTerms; i++) {
      ASSERT_TRUE(tenum.seekForward(ids[i])) << "failed at term " << ids[i] << " (index " << i << ")";
      ASSERT_EQ(tenum.term(), ids[i]);
      DocsOnlyEnum de(tenum);
      ASSERT_EQ(de.next(), i);
    }
  }

  // Test: alternating found/not-found with postings iteration (sparse delete pattern)
  {
    TermsEnum tenum(pool, reader, fieldInfo);
    ASSERT_TRUE(tenum.seek(ids[0]));
    DocsOnlyEnum de0(tenum);
    ASSERT_EQ(de0.next(), 0);

    for (int i = 1; i < nTerms; i++) {
      // Seek for a missing ID between ids[i-1] and ids[i]
      std::string missing = ids[i-1] + "x";
      if (missing < ids[i]) {  // only test if it's actually between them
        ASSERT_FALSE(tenum.seekForward(missing)) << "should miss " << missing;
      }

      ASSERT_TRUE(tenum.seekForward(ids[i])) << "should find " << ids[i];
      ASSERT_EQ(tenum.term(), ids[i]);
      DocsOnlyEnum de(tenum);
      ASSERT_EQ(de.next(), i);
    }
  }

  // Test: seek only even-indexed terms (skip every other)
  {
    TermsEnum tenum(pool, reader, fieldInfo);
    bool first = true;
    for (int i = 0; i < nTerms; i += 2) {
      bool found = first ? tenum.seek(ids[i]) : tenum.seekForward(ids[i]);
      first = false;
      ASSERT_TRUE(found) << "skip-2 failed for " << ids[i];
      ASSERT_EQ(tenum.term(), ids[i]);
      DocsOnlyEnum de(tenum);
      ASSERT_EQ(de.next(), i);
    }
  }
}

// Random fuzzer for seekForward, modeled on what applyDeletes actually does:
// a per-segment RANDOM SUBSET of variable-width numeric ids (so block
// boundaries land at random terms), sought via random-gap sorted forward
// sequences from a FRESH enum (applyDeletes dropped the initial full seek).
// The structured seekForward tests use fixed id sets and fixed strides and
// miss this; this is the gap that let a seekForward bug ship in applyDeletes.
TEST_F(PostingsTest, seekForwardRandom) {
  for (int trial = 0; trial < 100; trial++) {
    RAMDir dir;
    MemPool pool;

    // Random subset of numeric ids -> variable widths, random block boundaries.
    int wanted = rng.rint(1, Postings::TERMS_BLOCK_SIZE * 3);
    int range = rng.rint(wanted, wanted * 50 + 10);
    std::set<std::string> idset;
    while ((int)idset.size() < wanted) {
      idset.insert(std::to_string(rng.rint(0, range)));
    }
    std::vector<std::string> ids(idset.begin(), idset.end());  // lexically sorted
    int nTerms = (int)ids.size();

    PostingsWriter postingsWriter(dir, 0, nTerms);
    {
      TextWriter writer(postingsWriter);
      writer.startField("id");
      for (int i = 0; i < nTerms; i++) {
        TermRef term(pool, ids[i].data(), ids[i].size());
        writer.startTerm(term);
        writer.startDoc(i);
        writer.addPositionDelta(1);
        writer.endDoc(i);
        writer.endTerm(term);
      }
      writer.endField();
    }
    postingsWriter.finish();

    PostingsReader reader(dir, 0);
    FieldReader fieldReader(reader);
    ASSERT_TRUE(fieldReader.readNextField());
    SegFieldInfo fieldInfo;
    fieldReader.readFieldInfo(fieldInfo);

    int seqs = rng.rint(1, 20);
    for (int s = 0; s < seqs; s++) {
      TermsEnum tenum(pool, reader, fieldInfo);
      // Build a SORTED query sequence mixing present ids and (mostly) absent
      // values - exactly like applyDeletes seeking a sorted delete span whose
      // ids may or may not exist in this segment.  Misses interleaved with hits
      // are the untested case.
      int nq = rng.rint(1, nTerms + 5);
      std::vector<std::string> queries;
      for (int q = 0; q < nq; q++) {
        if (nTerms > 0 && rng.rint(2) == 0) {
          queries.push_back(ids[rng.rint(0, nTerms)]);          // present (hit)
        } else {
          queries.push_back(std::to_string(rng.rint(0, range))); // maybe absent (miss)
        }
      }
      std::sort(queries.begin(), queries.end());  // byte-lexical, same as term order

      // Optionally start positioned with a full seek (mixed usage).
      if (rng.rint(3) == 0 && nTerms > 0) {
        ASSERT_TRUE(tenum.seek(ids[0]));
      }
      for (const auto& q : queries) {
        bool found = tenum.seekForward(q);
        auto it = std::lower_bound(ids.begin(), ids.end(), q);
        bool present = (it != ids.end() && *it == q);
        int idx = (int)(it - ids.begin());
        ASSERT_EQ(found, present) << "trial=" << trial << " seq=" << s
                           << " q='" << q << "' expectedOrd=" << (present ? idx : -1)
                           << " nTerms=" << nTerms;
        if (found) {
          ASSERT_EQ(tenum.ord(), idx) << "trial=" << trial << " seq=" << s << " q='" << q << "'";
          ASSERT_EQ(tenum.term(), q) << "trial=" << trial << " seq=" << s << " q='" << q << "'";
          DocsOnlyEnum de(tenum);
          ASSERT_EQ(de.next(), idx) << "trial=" << trial << " seq=" << s << " q='" << q << "'";
        }
      }
    }
  }
}

TEST_F(PostingsTest, randTail) {
  // avoid creating a full block of terms, docs, or positions
  positionsPerDocMax = 11;
  docsPerTermMax = 11;
  termsPerFieldMax = 120;
  for (int64_t i = 0; i < scaleTestWork(3); i++) {
    auto nFields = rng.rint(1,20);
    initWriter();
    addFields(false, nFields);
    initReader();
    addFields(true, nFields);
  }
}

TEST_F(PostingsTest, randManyPos) {
  for (int64_t i = 0; i < scaleTestWork(3); i++) {
    auto nFields = rng.rint(1,20);
    positionsPerDocMax = Postings::POSITIONS_BLOCK_SIZE * 5/2;
    docsPerTermMax = 120;  // less than a doc block
    termsPerFieldMax = 2;

    initWriter();
    addFields(false, nFields);
    initReader();
    addFields(true, nFields);
  }
}

TEST_F(PostingsTest, randManyDocPos) {
  for (int64_t i = 0; i < scaleTestWork(3); i++) {
    auto nFields = rng.rint(1,20);
    positionsPerDocMax = Postings::POSITIONS_BLOCK_SIZE * 5/2;
    docsPerTermMax = Postings::DOCS_BLOCK_SIZE * 5/2;
    termsPerFieldMax = 2;

    initWriter();
    addFields(false, nFields);
    initReader();
    addFields(true, nFields);
  }
}

// test enough terms that more than one term block per field is required
TEST_F(PostingsTest, randManyTerms) {
  positionsPerDocMax = 3;
  docsPerTermMax = 3;
  termsPerFieldMax = Postings::TERMS_BLOCK_SIZE * 5 / 2;

  for (int64_t i = 0; i < scaleTestWork(3); i++) {
    auto nFields = rng.rint(1,20);
    initWriter();
    addFields(false, nFields);
    initReader();
    addFields(true, nFields);
  }
}

#if REMOVED
// An indispensable example of how to come up with a very small test case that fails.  Run many times with multiple seeds
// and if one fails, then set the lower bound of the loop to that seed number and debug!
TEST_F(PostingsTest, randWriteTmp) {
  std::cout << "SEED=" << rng_seed << std::endl;

  int nFields = 1;

  // this set is good for finding bugs with pulsed and skipping  (just 1 term, 1 or 2 docs, 1 or 2 positions)
  positionsPerDocMax = 3; docsPerTermMax = 3; termsPerFieldMax = 2;

  // this set is good for finding bugs with position blocks mixed in and skipping
  positionsPerDocMax = Postings::POSITIONS_BLOCK_SIZE*3/2; docsPerTermMax = 3; termsPerFieldMax = 2;

  // doc blocks
  positionsPerDocMax = 3; docsPerTermMax = Postings::DOCS_BLOCK_SIZE*2; termsPerFieldMax = 2;

  // term blocks
  positionsPerDocMax = 3; docsPerTermMax = 3; termsPerFieldMax = Postings::TERMS_BLOCK_SIZE*5/2;

  // multiple fields
  nFields=2;

  for (int i=1; i<1000; i++) {
    std::cout << "seed " << i << std::endl;
    rng.init(i);
    initWriter();
    addFields(false, nFields);
    initReader();
    addFields(true, nFields);
  }
}
#endif



TEST_F(PostingsTest, intCol) {
  RAMDir dir;
  MemPool pool;
  auto fname1 = PackedTerm(pool, "ifield1");

  {
    auto guard = pool.rewindScopeGuard();

    PostingsWriter writer(dir, 0, 3);

    auto &finfo = writer.addField(fname1);
    {
      auto outputPtr = writer.getOutputStream();
      IntColWriter colWriter(*outputPtr);
      colWriter.addInt64(77);
      colWriter.addInt64(33);
      colWriter.addInt64(11);
      colWriter.finish(finfo);
    }
    {
      DocsWithValWriter docsWriter(pool, writer, finfo);
      docsWriter.startDoc(0);
      docsWriter.startDoc(1);
      docsWriter.startDoc(2);
      docsWriter.finish();
    }

    writer.finish();
  }

  // All this should have been able to be done in a single segment file.
  std::vector<std::string> files;
  dir.listFiles(files);
  ASSERT_EQ(files.size(), 1);

  PostingsReader reader(dir, 0);
  FieldReader fieldReader(reader);
  ASSERT_TRUE(fieldReader.readNextField());
  ASSERT_EQ(fieldReader.name(), fname1);
  SegFieldInfo fieldInfo;
  fieldReader.readFieldInfo(fieldInfo);

  IntColReader colReader(reader, fieldInfo);
  ASSERT_EQ(colReader.docsWithValue(), 3);

  {
    IntColReader::SparseValues iter(colReader);
    ASSERT_EQ(iter.next(), 0);
    ASSERT_EQ(iter.index(), 0);
    ASSERT_EQ(iter.value(), 77);
    ASSERT_EQ(iter.next(), 1);
    ASSERT_EQ(iter.index(), 1);
    ASSERT_EQ(iter.value(), 33);
    ASSERT_EQ(iter.next(), 2);
    ASSERT_EQ(iter.index(), 2);
    ASSERT_EQ(iter.value(), 11);
    ASSERT_EQ(iter.next(), IntColReader::ENDINDEX);
    ASSERT_EQ(iter.index(), IntColReader::ENDINDEX);
  }

  {
    IntColReader::Iterator iter(colReader);
    ASSERT_EQ(iter.next(), 0);
    ASSERT_EQ(iter.docId(), 0);
    ASSERT_EQ(iter.value(), 77);
    ASSERT_EQ(iter.next(), 1);
    ASSERT_EQ(iter.docId(), 1);
    ASSERT_EQ(iter.value(), 33);
    ASSERT_EQ(iter.next(), 2);
    ASSERT_EQ(iter.docId(), 2);
    ASSERT_EQ(iter.value(), 11);
    ASSERT_EQ(iter.next(), IntColReader::ENDDOC);
    ASSERT_EQ(iter.docId(), IntColReader::ENDDOC);
  }
}




} // end namespace
