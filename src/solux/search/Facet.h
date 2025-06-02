#pragma once
#include "protos/solux_types.pb.h"
#include <string_view>
#include <vector>
#include <gtl/btree.hpp>
#include <boost/unordered/unordered_flat_map.hpp>

#include "DocSet.h"
#include "IndexReader.h"
#include "solux/reader/IntColReader.h"
#include "solux/schema/Schema.h"

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
  int missing_num = 0;
public:
  std::string_view facetName;
  std::vector<boost::unordered_flat_map<int64_t, int64_t>> allCounts;

  static FacetReq* createFieldFacetReq(Schema &schema, std::string_view facetName, const proto::FieldFacet& facetReq, google::protobuf::Arena& arena, IndexReader& reader);

  FacetReq(IndexReader& reader, std::string_view fieldName, std::string_view facetName, int64_t limit, bool missing)
  : reader(reader), fieldName(fieldName), facetName(facetName), limit(limit), missing(missing) {
    allCounts.resize(reader.segments().size());
  }
  virtual ~FacetReq() = default;
  virtual void facetSeg(FacetDomain& domain, int32_t segnum) = 0; // facet a single segment
  virtual void facetResult(solux::proto::FacetResult& facetResultProto) = 0; // merge all segments and fill in the result proto
};

class IntFacetBaseReq : public FacetReq {
public:
  IntFacetBaseReq(IndexReader& reader, std::string_view fieldName, std::string_view facetName, int64_t limit, bool missing) :
  FacetReq(reader, fieldName, facetName, limit, missing){}

  virtual ~IntFacetBaseReq() = default;

  void facetSeg(FacetDomain& domain, int32_t segnum) {
    std::vector<bool>& matches = domain.allMatches[segnum].docs;
    boost::unordered_flat_map<int64_t, int64_t>& count = allCounts[segnum];
    auto& postingsReader = reader.segments()[segnum].postingsReader();
    auto poolGuard = MemPool::threadLocalPoolGuard();
    FieldReader fieldReader(poolGuard.pool(), postingsReader);
    bool found = fieldReader.seek(fieldName);
    if (!found) {
      missing_num += std::count(matches.begin(), matches.end(), true);
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
  }
};

class IntFacetReq : public IntFacetBaseReq {
public:
  IntFacetReq(IndexReader& reader, std::string_view fieldName, std::string_view facetName, int64_t limit, bool missing) :
  IntFacetBaseReq(reader, fieldName, facetName, limit, missing){}

  void facetResult(solux::proto::FacetResult& facetResultProto) {
    // merge all the counts from all segments into the largest map
    auto& counts = allCounts[0];
    for (size_t i = 1; i < allCounts.size(); i++) {
      auto& segCounts = allCounts[i];
      // its faster to merge the smaller map into the larger one
      if (segCounts.size() > counts.size()) {
        std::swap(counts, segCounts);
      }
      for (auto [val, count] : segCounts) {
        counts[val] += count;
      }
      segCounts.clear();
    }
    std::vector<std::pair<int64_t, int64_t>> countVec;
    for (auto [val, count] : counts) {
      countVec.emplace_back(val, count);
    }
    counts.clear();
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
      facetResultProto.set_missing(missing_num);
    }


  }
};

class StrFacetReq : public IntFacetBaseReq {
public:
  StrFacetReq(IndexReader& reader, std::string_view fieldName, std::string_view facetName, int64_t limit, bool missing) :
  IntFacetBaseReq(reader, fieldName, facetName, limit, missing){}

  void facetResult(solux::proto::FacetResult& facetResultProto) {
    // merge all the counts from all segments into the largest map
    boost::unordered_flat_map<std::string, int64_t> counts;

    for (size_t i = 0; i < allCounts.size(); i++) {
      auto& segCounts = allCounts[i];
      if (segCounts.empty()) {
        continue; // no counts for this segment
      }
      auto& postingsReader = reader.segments()[i].postingsReader();
      auto poolGuard = MemPool::threadLocalPoolGuard();
      FieldReader fieldReader(poolGuard.pool(), postingsReader);
      SegFieldInfo segFieldInfo;
      bool found = fieldReader.seek(fieldName);
      if (!found) {
        continue;
      }
      fieldReader.readFieldInfo(segFieldInfo);
      TermsEnum tenum(poolGuard.pool(), postingsReader, segFieldInfo);
      for (auto [ord, count] : segCounts) {
        tenum.seekOrd(ord - 1);
        std::string_view termView = (std::string_view) tenum.term();
        counts[std::string(termView)] += count;
      }
      segCounts.clear();
    }

    std::vector<std::pair<std::string, int64_t>> countVec;
    for (auto [val, count] : counts) {
      countVec.emplace_back(val, count);
    }
    counts.clear();
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
      facetResultProto.set_missing(missing_num);
    }


  }
};


}
