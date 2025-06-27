#pragma once
#include <string_view>
#include <vector>
#include <boost/unordered/unordered_flat_map.hpp>

#include "protos/solux_types.pb.h"
#include "solux/search/DocSet.h"
#include "solux/search/IndexReader.h"
#include "SearchOp.h"
#include "solux/reader/DocsEnum.h"
#include "solux/reader/IntColReader.h"
#include "solux/reader/TermsEnum.h"
#include "solux/schema/Schema.h"
#include "solux/util/AtomicMerger.h"

namespace solux {
class FacetDomain {
public:
  std::vector<BitDocSet> allMatches;
};

class FacetReq : public SearchOp {
public:
  IndexReader& reader;
  std::string_view fieldName;
  int64_t limit;
  int64_t minCount; // minimum count for a facet to be included in the result
  bool missing;

  std::string_view facetName;

  static FacetReq* createFieldFacetReq(SearchRequest& req, std::string_view facetName, const proto::FieldFacet& facetReq,
                                        google::protobuf::Arena& arena);

  FacetReq(SearchRequest& req, std::string_view fieldName, std::string_view facetName, int64_t limit, int64_t minCount, bool missing)
    : SearchOp(req, facetName), reader(*req.reader), fieldName(fieldName), limit(limit), minCount(minCount), missing(missing),
      facetName(facetName) {
  }

  virtual ~FacetReq() = default;

  // utility template method that calls callback with (int32 docid, int64_t value) for each doc in the domain that has
  // a value in the int column field (single or multi-valued).
  // missing is an out parameter that is incremented for every domain doc that does not have the field.
  // segFieldInfo is passed in uninitializsed and filled in if the field exists in the segment.
  // The value returned is if the field exists in the segment.
  bool facetSegIntCol(DocSet* domain, int32_t segnum, int64_t& missing_num, SegFieldInfo& segFieldInfo, auto&& callback) {
    BitDocSet* bitDocs = (BitDocSet*) domain;
    auto* domainBits = bitDocs ? &bitDocs->bits() : nullptr;

    auto& postingsReader = reader.segments()[segnum].postingsReader();
    int32_t maxDoc = postingsReader.maxDoc();
    auto poolGuard = MemPool::threadLocalPoolGuard();
    FieldReader fieldReader(poolGuard.pool(), postingsReader);
    bool found = fieldReader.seek(fieldName);
    if (!found) {
      if (bitDocs) {
        missing_num += bitDocs->card();
      } else {
        missing_num += maxDoc;
      }
      return false;
    }
    fieldReader.readFieldInfo(segFieldInfo);
    // this is a int field for now, so we need to read the value for each doc
    // and accumulate counts per value.
    IntColReader intColReader(poolGuard.pool(), postingsReader, segFieldInfo);
    IntColReader::Iterator intColIter(intColReader);
    for (int32_t docid = 0; docid < maxDoc; docid++) {
      if (bitDocs && !bitDocs->get(docid)) {
        continue;
      }
      if (intColIter.docId() < docid ) {
        intColIter.advance(docid);
      }
      if (intColIter.docId() == docid) {
        if (!intColReader.multiValued()) {
          auto val = intColIter.value();
          callback(docid, val);
        } else {
          auto [start, end] = intColReader.getStartEndRank(intColIter.rank());
          auto n = end - start;
          for (int64_t vrank = 0; vrank < n; vrank++) {
            auto val = intColIter.values().valueAt(start + vrank);
            callback(docid, val);
          }
        }
      } else {
        missing_num++;
      }
    }
    return true;
  }
};

class IntFacetBaseReq : public FacetReq {
public:
  IntFacetBaseReq(SearchRequest& req, std::string_view fieldName, std::string_view facetName, int64_t limit, int64_t minCount, bool missing) :
  FacetReq(req, fieldName, facetName, limit, minCount, missing){}

  virtual ~IntFacetBaseReq() = default;

};

class IntFacetReq : public IntFacetBaseReq {
public:
  class MergeableIntFacet : public MergeableData {
  public:
    boost::unordered_flat_map<int64_t, int64_t> counts;
    int64_t missing_num = 0; // number of missing values in this segment

    static MergeableIntFacet* merge(MergeableIntFacet* a, MergeableIntFacet* b) {
      // merge the smaller collector into the larger collector, or if both the same size, merge
      // the less competitive collector into the more competitive collector.
      if (a->counts.size() < b->counts.size()) {
        std::swap(a,b);
      }

      for (auto [val, count] : b->counts) {
        a->counts[val] += count;
      }
      a->missing_num += b->missing_num;
      return a;
    }
  };
private:
  AtomicMerger<MergeableIntFacet> countMerger;
public:
  IntFacetReq(SearchRequest& req, std::string_view fieldName, std::string_view facetName, int64_t limit, int64_t minCount, bool missing) :
  IntFacetBaseReq(req, fieldName, facetName, limit, minCount, missing){}

  class Calc : public Calculator {
    AtomicMerger<MergeableIntFacet> countMerger;
  public:
    Calc(SearchOp& op, Calculator* parent) : Calculator(op, parent){}
    IntFacetReq& thisOp() {
      return (IntFacetReq&)getOp();
    }

solux::proto::Val* getTargetForSub(solux::proto::SearchResponse* searchResponse, Calculator* sub) override {
  return nullptr;
};
    void calc(oneapi::tbb::task_group* tg, int32_t segnum, DocSet* domain) override {
      std::unique_ptr<MergeableIntFacet> mergeableData(countMerger.obtain());
      boost::unordered_flat_map<int64_t, int64_t>& count = mergeableData->counts;
      SegFieldInfo segFieldInfo;
      auto& facetReq = (FacetReq&)getOp();
      facetReq.facetSegIntCol(domain, segnum, mergeableData->missing_num, segFieldInfo, [&](int32_t docid, int64_t val) {
        unused(docid);
        count[val]++;
      });
      auto merged = countMerger.release(mergeableData.release());
      if (merged == thisOp().reader.segments().size()) {
        facetResult();
      }
    };

    void facetResult() {
      auto* myVal = getTarget(nullptr);
      solux::proto::FacetResult& facetResultProto = *myVal->mutable_facet();
      auto minCount = thisOp().minCount;
      auto limit = thisOp().limit;
      auto missing = thisOp().missing;

      auto* mergedData = countMerger.obtain();
      auto& counts = mergedData->counts;
      std::vector<std::pair<int64_t, int64_t>> countVec;
      for (auto [val, count] : counts) {
        if (count >= minCount) {
          countVec.emplace_back(val, count);
        }
      }
      auto missing_count = mergedData->missing_num;
      delete mergedData;
      std::sort(countVec.begin(), countVec.end(), [](const auto& a, const auto& b) {
        if (a.second != b.second ) {
          return a.second > b.second;
        }
        return a.first < b.first;
      });
      if (limit >= 0 && limit < (int64_t)countVec.size()) {
        countVec.resize(limit);
      }

      // fill in the facet result proto
      auto& bucketIds = *facetResultProto.mutable_bucket_ids()->mutable_col_i();
      auto& bucketIdsArr = *bucketIds.mutable_v();
      auto& countsArr = *facetResultProto.mutable_counts();
      bucketIdsArr.Reserve(countVec.size());
      countsArr.Reserve(countVec.size());
      for (auto [val, count] : countVec) {
        bucketIdsArr.Add(val);
        countsArr.Add(count);
      }
      if (missing) {
        facetResultProto.set_missing(missing_count);
      }


    }
  };

  Calculator* createCalculator(Calculator* parent, int64_t slot) override {
    return new Calc(*this, parent);
  };


};

class StrFacetReq : public IntFacetBaseReq {
  class MergeableStrFacet : public MergeableData {
  public:
    boost::unordered_flat_map<std::string, int64_t> counts;
    int64_t missing_num = 0; // number of missing values in this segment

    static MergeableStrFacet* merge(MergeableStrFacet* a, MergeableStrFacet* b) {
      // merge the smaller collector into the larger collector, or if both the same size, merge
      // the less competitive collector into the more competitive collector.
      if (a->counts.size() < b->counts.size()) {
        std::swap(a,b);
      }

      for (auto [val, count] : b->counts) {
        a->counts[val] += count;
      }
      a->missing_num += b->missing_num;
      return a;
    }
  };

public:
  StrFacetReq(SearchRequest& req, std::string_view fieldName, std::string_view facetName, int64_t limit, int64_t minCount, bool missing) :
  IntFacetBaseReq(req, fieldName, facetName, limit, minCount, missing){}

  class Calc : public Calculator {
    AtomicMerger<MergeableStrFacet> countMerger;
  public:
    Calc(SearchOp& op, Calculator* parent) : Calculator(op, parent){}
    StrFacetReq& thisOp() {
      return (StrFacetReq&)getOp();
    }

    solux::proto::Val* getTargetForSub(solux::proto::SearchResponse* searchResponse, Calculator* sub) override {
      return nullptr;
    };
    void calc(oneapi::tbb::task_group* tg, int32_t segnum, DocSet* domain) override {
      SegFieldInfo segFieldInfo;
      boost::unordered_flat_map<int64_t, int64_t> count;
      int64_t missing_num = 0;
      auto& facetReq = (FacetReq&)getOp();
      facetReq.facetSegIntCol(domain, segnum, missing_num, segFieldInfo,
        [&](int32_t docid, int64_t val) {
          unused(docid);
          count[val]++;
        });

      if (count.empty()) {
        // no values found, so we can just return
        if (missing_num > 0) {
          auto* mergeableData = countMerger.obtain();
          mergeableData->missing_num += missing_num;
          countMerger.release(mergeableData);
        }
        return;
      }
      PostingsReader& postingsReader = thisOp().reader.segments()[segnum].postingsReader();
      auto poolGuard = MemPool::threadLocalPoolGuard();

      std::unique_ptr<MergeableStrFacet> mergeableData(countMerger.obtain());
      mergeableData->missing_num += missing_num;
      TermsEnum tenum(poolGuard.pool(), postingsReader, segFieldInfo);
      for (auto [ord, count] : count) {
        tenum.seekOrd(ord - 1);
        std::string_view termView = (std::string_view) tenum.term();
        mergeableData->counts[std::string(termView)] += count;
      }
      auto merged = countMerger.release(mergeableData.release());
      if (merged == thisOp().reader.segments().size()) {
        facetResult();
      }
    };

    void facetResult() {
      auto* myVal = getTarget(nullptr);
      solux::proto::FacetResult& facetResultProto = *myVal->mutable_facet();
      auto minCount = thisOp().minCount;
      auto limit = thisOp().limit;
      auto missing = thisOp().missing;

      auto* mergedData = countMerger.obtain();
      auto& counts = mergedData->counts;

      std::vector<std::pair<std::string, int64_t>> countVec;
      for (auto [val, count] : counts) {
        if (minCount == -1 || count >= minCount) {
          countVec.emplace_back(val, count);
        }
      }
      auto missing_count = mergedData->missing_num;
      delete mergedData;
      std::sort(countVec.begin(), countVec.end(), [](auto& a, auto& b) {
        if (a.second != b.second ) {
          return a.second > b.second;
        }
        return a.first < b.first;
      });
      if (limit >= 0 && limit < (int64_t)countVec.size()) {
        countVec.resize(limit);
      }

      // fill in the facet result proto
      auto& bucketIds = *facetResultProto.mutable_bucket_ids()->mutable_col_s();
      auto& bucketIdsArr = *bucketIds.mutable_v();
      auto& countsArr = *facetResultProto.mutable_counts();
      bucketIdsArr.Reserve(countVec.size());
      countsArr.Reserve(countVec.size());
      for (auto [val, count] : countVec) {
        auto* strptr = bucketIdsArr.Add();
        *strptr = val; // copy the string
        countsArr.Add(count);
      }
      if (missing) {
        facetResultProto.set_missing(missing_count);
      }


    }
  };
  Calculator* createCalculator(Calculator* parent, int64_t slot) override {
    return new Calc(*this, parent);
  };
};

class FullTextFacetReq : public FacetReq {
  class MergeableStrFacet : public MergeableData {
  public:
    boost::unordered_flat_map<std::string, int64_t> counts;
    int64_t missing_num = 0; // number of missing values in this segment

    static MergeableStrFacet* merge(MergeableStrFacet* a, MergeableStrFacet* b) {
      // merge the smaller collector into the larger collector, or if both the same size, merge
      // the less competitive collector into the more competitive collector.
      if (a->counts.size() < b->counts.size()) {
        std::swap(a,b);
      }

      for (auto [val, count] : b->counts) {
        a->counts[val] += count;
      }
      a->missing_num += b->missing_num;
      return a;
    }
  };

public:
  FullTextFacetReq(SearchRequest& req, std::string_view fieldName, std::string_view facetName, int64_t limit, int64_t minCount, bool missing) :
  FacetReq(req, fieldName, facetName, limit, minCount, missing){}

  class Calc : public Calculator {
    AtomicMerger<MergeableStrFacet> countMerger;
  public:
    Calc(SearchOp& op, Calculator* parent) : Calculator(op, parent){}
    StrFacetReq& thisOp() {
      return (StrFacetReq&)getOp();
    }

    solux::proto::Val* getTargetForSub(solux::proto::SearchResponse* searchResponse, Calculator* sub) override {
      return nullptr;
    };
    void calc(oneapi::tbb::task_group* tg, int32_t segnum, DocSet* domain) override {
      SegFieldInfo segFieldInfo;
      int64_t missing_num = 0;
      auto& facetReq = (FacetReq&)getOp();
      BitDocSet* bitDocs = (BitDocSet*) domain;
      auto* domainBits = bitDocs ? &bitDocs->bits() : nullptr;
      std::unique_ptr<MergeableStrFacet> mergeableData(countMerger.obtain());
      boost::unordered_flat_map<std::string, int64_t>& counts = mergeableData->counts;

      auto& postingsReader = thisOp().reader.segments()[segnum].postingsReader();
      int32_t maxDoc = postingsReader.maxDoc();
      auto poolGuard = MemPool::threadLocalPoolGuard();
      FieldReader fieldReader(poolGuard.pool(), postingsReader);
      bool found = fieldReader.seek(thisOp().fieldName);
      if (!found) {
        if (bitDocs) {
          mergeableData->missing_num += bitDocs->card();
        } else {
          mergeableData->missing_num += maxDoc;
        }
        return;
      }
      fieldReader.readFieldInfo(segFieldInfo);
      TermsEnum tenum(poolGuard.pool(), postingsReader, segFieldInfo);
      while (tenum.nextTerm()) {
        int64_t count = 0;
        DocsEnum denum(poolGuard.pool(), postingsReader, tenum);
        while (true) {
          auto doc = denum.nextDoc();
          if (doc == DocsEnum::END) {
            break; // no more docs for this term
          }
          if (domainBits && !domainBits->get(doc)) {
            continue; // this doc is not in the domain
          }
          count++;
        }
        // use heterogeneous lookup in the future to avoid creating string when not needed
        counts[(std::string) (std::string_view) tenum.term()] += count;
      }
      auto merged = countMerger.release(mergeableData.release());
      if (merged == thisOp().reader.segments().size()) {
        facetResult();
      }
    }

    void facetResult() {
      auto* myVal = getTarget(nullptr);
      solux::proto::FacetResult& facetResultProto = *myVal->mutable_facet();
      auto minCount = thisOp().minCount;
      auto limit = thisOp().limit;
      auto missing = thisOp().missing;

      auto* mergedData = countMerger.obtain();
      auto& counts = mergedData->counts;

      std::vector<std::pair<std::string, int64_t>> countVec;
      for (auto [val, count] : counts) {
        if (minCount == -1 || count >= minCount) {
          countVec.emplace_back(val, count);
        }
      }
      auto missing_count = mergedData->missing_num;
      delete mergedData;
      std::sort(countVec.begin(), countVec.end(), [](auto& a, auto& b) {
        if (a.second != b.second ) {
          return a.second > b.second;
        }
        return a.first < b.first;
      });
      if (limit >= 0 && limit < (int64_t)countVec.size()) {
        countVec.resize(limit);
      }

      // fill in the facet result proto
      auto& bucketIds = *facetResultProto.mutable_bucket_ids()->mutable_col_s();
      auto& bucketIdsArr = *bucketIds.mutable_v();
      auto& countsArr = *facetResultProto.mutable_counts();
      bucketIdsArr.Reserve(countVec.size());
      countsArr.Reserve(countVec.size());
      for (auto [val, count] : countVec) {
        auto* strptr = bucketIdsArr.Add();
        *strptr = val; // copy the string
        countsArr.Add(count);
      }
      if (missing) {
        facetResultProto.set_missing(missing_count);
      }
    }


    };
  Calculator* createCalculator(Calculator* parent, int64_t slot) override {
    return new Calc(*this, parent);
  }
};

class IntFacetRangeReq : public FacetReq {
  int64_t start;
  int64_t end;
  int64_t gap;
public:
  IntFacetRangeReq(SearchRequest& req, std::string_view fieldName, std::string_view facetName, int64_t start, int64_t end, int64_t gap, int64_t minCount, bool missing)
  : FacetReq(req, fieldName, facetName, -1, minCount, missing), start(start), end(end), gap(gap) {}

  virtual ~IntFacetRangeReq() = default;

  class Calc : public Calculator {
    AtomicMerger<IntFacetReq::MergeableIntFacet> countMerger;
  public:
    Calc(SearchOp& op, Calculator* parent) : Calculator(op, parent){}
    IntFacetRangeReq& thisOp() {
      return (IntFacetRangeReq&)getOp();
    }

    solux::proto::Val* getTargetForSub(solux::proto::SearchResponse* searchResponse, Calculator* sub) override {
      return nullptr;
    };
    void calc(oneapi::tbb::task_group* tg, int32_t segnum, DocSet* domain) override {
      std::unique_ptr<IntFacetReq::MergeableIntFacet> mergeableData(countMerger.obtain());
      SegFieldInfo segFieldInfo;
      boost::unordered_flat_map<int64_t, int64_t>& count = mergeableData->counts;
      auto start = thisOp().start;
      auto end = thisOp().end;
      auto gap = thisOp().gap;
      auto& facetReq = (FacetReq&)getOp();
      facetReq.facetSegIntCol(domain, segnum, mergeableData->missing_num, segFieldInfo, [&](int32_t docid, int64_t val) {
        unused(docid);
        if (val < start || val >= end) {
          return; // value is out of range
        }
        count[(val-start)/gap]++;
      });
      auto merged = countMerger.release(mergeableData.release());
      if (merged == thisOp().reader.segments().size()) {
        facetResult();
      }
    };

    void facetResult() {
      auto* myVal = getTarget(nullptr);
      solux::proto::FacetResult& facetResultProto = *myVal->mutable_facet();
      auto minCount = thisOp().minCount;
      auto limit = thisOp().limit;
      auto missing = thisOp().missing;
      auto start = thisOp().start;
      auto end = thisOp().end;
      auto gap = thisOp().gap;

      auto* mergedData = countMerger.obtain();
      auto& counts = mergedData->counts;
      std::vector<std::pair<int64_t, int64_t>> countVec;
      for (auto [val, count] : counts) {
        if (minCount == -1 || count >= minCount) {
          countVec.emplace_back(val, count);
        }
      }
      auto missing_count = mergedData->missing_num;
      delete mergedData;
      std::sort(countVec.begin(), countVec.end(), [](auto& a, auto& b) {
        return a.first < b.first;
      });
      if (limit >= 0 && limit < (int64_t)countVec.size()) {
        countVec.resize(limit);
      }

      // fill in the facet result proto
      auto& bucketIds = *facetResultProto.mutable_bucket_ids()->mutable_multi_i();
      auto& bucketIdsArr = *bucketIds.mutable_v();
      auto& countsArr = *facetResultProto.mutable_counts();
      bucketIdsArr.Reserve(countVec.size());
      countsArr.Reserve(countVec.size());
      for (auto [val, count] : countVec) {
        auto& pair = *bucketIdsArr.Add();
        pair.mutable_v()->Add(start + val * gap);
        int64_t bucketEnd = start + (val + 1) * gap;
        pair.mutable_v()->Add(std::min(bucketEnd, end));
        countsArr.Add(count);
      }
      if (missing) {
        facetResultProto.set_missing(missing_count);
      }


    }
  };
  Calculator* createCalculator(Calculator* parent, int64_t slot) override {
    return new Calc(*this, parent);
  };
};

inline FacetReq* FacetReq::createFieldFacetReq(SearchRequest& req, std::string_view facetName, const proto::FieldFacet& facetReq,
                                        google::protobuf::Arena& arena) {
  auto facetField = facetReq.field();
  int64_t limit = 5; // default limit
  if (facetReq.has_limit()) {
    limit = facetReq.limit();
  }
  int64_t minCount = -1;
  if (facetReq.has_mincount()) {
    minCount = facetReq.mincount();
  }
  auto missing = facetReq.missing();
  //arena allocate FacetReq
  FacetReq* facet = nullptr;
  auto& ftype = req.schema->getFieldTypeEx(facetField);

  switch (ftype->type()) {
    case FieldType::Type::INT:
      facet = google::protobuf::Arena::Create<IntFacetReq>(&arena, req, facetField, facetName, limit, minCount,  missing);
      break;
    case FieldType::Type::STRING:
      facet = google::protobuf::Arena::Create<StrFacetReq>(&arena, req, facetField, facetName, limit, minCount, missing);
      break;
    case FieldType::Type::TEXT:
      facet = google::protobuf::Arena::Create<FullTextFacetReq>(&arena, req, facetField, facetName, limit, minCount, missing);
      break;
    default: ;
  }
  if (facet == nullptr) {
    throw std::runtime_error("Unknown facet field type: " + std::string(facetField));
  }
  return facet;
}

}