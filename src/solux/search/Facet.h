#pragma once
#include "protos/solux_types.pb.h"
#include <string_view>
#include <vector>

#include "IndexReader.h"

namespace solux {
class FacetReq {
  IndexReader& reader;
  std::string_view fieldName;
public:
  std::string_view facetName;
  std::vector<std::vector<bool>> allMatches;
  std::vector<boost::unordered_flat_map<int64_t, int64_t>> allCounts;

  FacetReq(IndexReader& reader, std::string_view fieldName, std::string_view facetName)
  : reader(reader), fieldName(fieldName), facetName(facetName) {
    allMatches.resize(reader.segments().size());
    allCounts.resize(reader.segments().size());
  }

  void facetSeg(int32_t segnum) {
    std::vector<bool>& matches = allMatches[segnum];
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

  void mergeCounts() {
    // merge all the counts from all segments into the first map
    auto& counts = allCounts[0];
    for (size_t i = 1; i < allCounts.size(); i++) {
      auto& segCounts = allCounts[i];
      for (auto [val, count] : segCounts) {
        counts[val] += count;
      }
    }
  }

  void facetResult(solux::proto::FacetResult& facetResultProto) {
    mergeCounts();

    // fill in the facet result proto
    auto& bucketIds = *facetResultProto.mutable_bucket_ids()->mutable_col_i();
    auto& bucketIdsArr = *bucketIds.mutable_v();
    auto& countsArr = *facetResultProto.mutable_counts();
    bucketIdsArr.Reserve(allCounts[0].size());
    countsArr.Reserve(allCounts[0].size());
    for (auto [val, count] : allCounts[0]) {
      bucketIdsArr.Add(val);
      countsArr.Add(count);
    }


  }
};


}
