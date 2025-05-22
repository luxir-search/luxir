#pragma once
#include "protos/solux_types.pb.h"
#include <string_view>
#include <vector>

#include "DocSet.h"
#include "IndexReader.h"

namespace solux {

class FacetDomain {
public:
  std::vector<DocSet> allMatches;
};

class FacetReq {
  IndexReader& reader;
  std::string_view fieldName;
  int64_t limit;
public:
  std::string_view facetName;
  std::vector<boost::unordered_flat_map<int64_t, int64_t>> allCounts;

  FacetReq(IndexReader& reader, std::string_view fieldName, std::string_view facetName, int64_t limit)
  : reader(reader), fieldName(fieldName), facetName(facetName), limit(limit) {
    allCounts.resize(reader.segments().size());
  }

  void facetSeg(FacetDomain& domain, int32_t segnum) {
    std::vector<bool>& matches = domain.allMatches[segnum].docs;
    boost::unordered_flat_map<int64_t, int64_t>& count = allCounts[segnum];
    auto& postingsReader = reader.segments()[segnum].postingsReader();
    auto poolGuard = MemPool::threadLocalPoolGuard();
    FieldReader fieldReader(poolGuard.pool(), postingsReader);
    bool found = fieldReader.seek(fieldName);
    if (!found) {
      return;
    }
    SegFieldInfo segFieldInfo;
    fieldReader.readFieldInfo(segFieldInfo);
    // this is a single valued int field for now, so we need to read the value for each doc
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
        auto val = intColIter.value();
        count[val]++;
      }
    }
  }

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


  }
};


}
