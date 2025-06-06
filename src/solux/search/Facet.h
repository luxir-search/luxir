#pragma once
#include <string_view>
#include <vector>
#include <boost/unordered/unordered_flat_map.hpp>

#include "protos/solux_types.pb.h"
#include "DocSet.h"
#include "IndexReader.h"
#include "solux/reader/IntColReader.h"
#include "solux/schema/Schema.h"
#include "solux/util/AtomicMerger.h"

namespace solux {

class FacetDomain {
public:
  std::vector<DocSet> allMatches;
};

class FacetReq {
protected:
  IndexReader& reader;
  std::string_view fieldName;
  int64_t limit;
  bool missing;
  int64_t minCount; // minimum count for a facet to be included in the result
public:
  std::string_view facetName;

  static FacetReq* createFieldFacetReq(Schema &schema, std::string_view facetName, const proto::FieldFacet& facetReq, google::protobuf::Arena& arena, IndexReader& reader);

  FacetReq(IndexReader& reader, std::string_view fieldName, std::string_view facetName, int64_t limit, int64_t minCount, bool missing)
  : reader(reader), fieldName(fieldName), facetName(facetName), limit(limit), minCount(minCount), missing(missing) {}
  virtual ~FacetReq() = default;
  virtual void facetSeg(FacetDomain& domain, int32_t segnum) = 0; // facet a single segment
  virtual void facetResult(solux::proto::FacetResult& facetResultProto) = 0; // merge all segments and fill in the result proto

  bool facetSegIntCol(FacetDomain& domain, int32_t segnum, int64_t& missing, auto callback) {
    std::vector<bool>& matches = domain.allMatches[segnum].docs;
    auto& postingsReader = reader.segments()[segnum].postingsReader();
    auto poolGuard = MemPool::threadLocalPoolGuard();
    FieldReader fieldReader(poolGuard.pool(), postingsReader);
    bool found = fieldReader.seek(fieldName);
    if (!found) {
      missing += std::count(matches.begin(), matches.end(), true);
      return false;
    }
    SegFieldInfo segFieldInfo;
    fieldReader.readFieldInfo(segFieldInfo);
    // this is a int field for now, so we need to read the value for each doc
    // and accumulate counts per value.
    IntColReader intColReader(poolGuard.pool(), postingsReader, segFieldInfo);
    IntColReader::Iterator intColIter(intColReader);
    for (int32_t docid = 0; docid < matches.size(); docid++) {
      if (!matches[docid]) {
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
        missing++;
      }
    }
    return true;
  }
};

class IntFacetBaseReq : public FacetReq {
public:
  IntFacetBaseReq(IndexReader& reader, std::string_view fieldName, std::string_view facetName, int64_t limit, int64_t minCount, bool missing) :
  FacetReq(reader, fieldName, facetName, limit, minCount, missing){}

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
  IntFacetReq(IndexReader& reader, std::string_view fieldName, std::string_view facetName, int64_t limit, int64_t minCount, bool missing) :
  IntFacetBaseReq(reader, fieldName, facetName, limit, minCount, missing){}

  void facetSeg(FacetDomain& domain, int32_t segnum) override {
    std::unique_ptr<MergeableIntFacet> mergeableData(countMerger.obtain());
    boost::unordered_flat_map<int64_t, int64_t>& count = mergeableData->counts;
    facetSegIntCol(domain, segnum, mergeableData->missing_num, [&](int32_t docid, int64_t val) {
      count[val]++;
    });
    countMerger.release(mergeableData.release());
  }

  void facetResult(solux::proto::FacetResult& facetResultProto) override {
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
    if (limit >= 0 && limit < countVec.size()) {
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
  AtomicMerger<MergeableStrFacet> countMerger;
public:
  StrFacetReq(IndexReader& reader, std::string_view fieldName, std::string_view facetName, int64_t limit, int64_t minCount, bool missing) :
  IntFacetBaseReq(reader, fieldName, facetName, limit, minCount, missing){}

  void facetSeg(FacetDomain& domain, int32_t segnum) override {
    std::vector<bool>& matches = domain.allMatches[segnum].docs;
    boost::unordered_flat_map<int64_t, int64_t> count;
    int64_t missing_num = 0;
    auto& postingsReader = reader.segments()[segnum].postingsReader();
    auto poolGuard = MemPool::threadLocalPoolGuard();
    FieldReader fieldReader(poolGuard.pool(), postingsReader);
    bool found = fieldReader.seek(fieldName);
    if (!found) {
      auto* mergeableData = countMerger.obtain();
      mergeableData->missing_num += std::count(matches.begin(), matches.end(), true);
      countMerger.release(mergeableData);
      return;
    }
    SegFieldInfo segFieldInfo;
    fieldReader.readFieldInfo(segFieldInfo);
    // this is a int field for now, so we need to read the value for each doc
    // and accumulate counts per value.
    IntColReader intColReader(poolGuard.pool(), postingsReader, segFieldInfo);
    IntColReader::Iterator intColIter(intColReader);
    for (int32_t docid = 0; docid < matches.size(); docid++) {
      if (!matches[docid]) {
        continue;
      }
      if (intColIter.docId() < docid ) {
        intColIter.advance(docid);
      }
      if (intColIter.docId() == docid) {
        if (!intColReader.multiValued()) {
          auto val = intColIter.value();
          count[val]++;
        } else {
          auto [start, end] = intColReader.getStartEndRank(intColIter.rank());
          auto n = end - start;
          for (int64_t vrank = 0; vrank < n; vrank++) {
            auto val = intColIter.values().valueAt(start + vrank);
            count[val]++;
          }
        }
      } else {
        missing_num++;
      }
    }
    auto* mergeableData = countMerger.obtain();
    mergeableData->missing_num += missing_num;
    TermsEnum tenum(poolGuard.pool(), postingsReader, segFieldInfo);
    for (auto [ord, count] : count) {
      tenum.seekOrd(ord - 1);
      std::string_view termView = (std::string_view) tenum.term();
      mergeableData->counts[std::string(termView)] += count;
    }
    countMerger.release(mergeableData);
  }

  void facetResult(solux::proto::FacetResult& facetResultProto) override {
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
    if (limit >= 0 && limit < countVec.size()) {
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

class IntFacetRangeReq : public FacetReq {
  int64_t start;
  int64_t end;
  int64_t gap;
  AtomicMerger<IntFacetReq::MergeableIntFacet> countMerger;
public:
  IntFacetRangeReq(IndexReader& reader, std::string_view fieldName, std::string_view facetName, int64_t start, int64_t end, int64_t gap, int64_t minCount, bool missing)
  : FacetReq(reader, fieldName, facetName, -1, minCount, missing), start(start), end(end), gap(gap) {}

  virtual ~IntFacetRangeReq() = default;

  void facetSeg(FacetDomain& domain, int32_t segnum) override {
    std::vector<bool>& matches = domain.allMatches[segnum].docs;
    auto* mergeableData = countMerger.obtain();
    boost::unordered_flat_map<int64_t, int64_t>& count = mergeableData->counts;
    auto& postingsReader = reader.segments()[segnum].postingsReader();
    auto poolGuard = MemPool::threadLocalPoolGuard();
    FieldReader fieldReader(poolGuard.pool(), postingsReader);
    bool found = fieldReader.seek(fieldName);
    if (!found) {
      mergeableData->missing_num += std::count(matches.begin(), matches.end(), true);
      countMerger.release(mergeableData);
      return;
    }
    SegFieldInfo segFieldInfo;
    fieldReader.readFieldInfo(segFieldInfo);
    // this is a int field for now, so we need to read the value for each doc
    // and accumulate counts per value.
    IntColReader intColReader(poolGuard.pool(), postingsReader, segFieldInfo);
    IntColReader::Iterator intColIter(intColReader);
    for (int32_t docid = 0; docid < matches.size(); docid++) {
      if (!matches[docid]) {
        continue;
      }
      if (intColIter.docId() < docid ) {
        intColIter.advance(docid);
      }
      if (intColIter.docId() == docid) {
        if (!intColReader.multiValued()) {
          auto val = intColIter.value();
          if (val >= start && val < end) {
            count[(val-start)/gap]++;
          }
        } else {
          auto [start, end] = intColReader.getStartEndRank(intColIter.rank());
          auto n = end - start;
          for (int64_t vrank = 0; vrank < n; vrank++) {
            auto val = intColIter.values().valueAt(start + vrank);
            if (val >= start && val < end) {
              count[(val-start)/gap]++;
            }
          }
        }
      } else {
        mergeableData->missing_num++;
      }
    }
    countMerger.release(mergeableData);
  }

  void facetResult(solux::proto::FacetResult& facetResultProto) override {
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
    if (limit >= 0 && limit < countVec.size()) {
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
      pair.mutable_v()->Add(start + (val + 1) * gap);
      countsArr.Add(count);
    }
    if (missing) {
      facetResultProto.set_missing(missing_count);
    }


  }
};


}
