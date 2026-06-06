#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>
#include <boost/unordered/unordered_flat_set.hpp>
#include "protos/solux_types.pb.h"
#include "solux/store/Directory.h"
#include "solux/schema/Schema.h"
#include "solux/reader/PostingsReader.h"

namespace solux {

/// Builds FAISS aux indexes for vector fields in one segment.
///
/// Flat kNN is served directly from the vector column.  Aux builds produce an
/// IVF+PQ ANN index when enough vectors are present and the segment is large
/// enough to be worth indexing.
///
/// One index per (eligible) vector field per segment, written under a single
/// filename produced by Postings::getAuxIndexFileName.  The "name" recorded in
/// the segment overlay is "vec.<fieldName>" (e.g. "vec.title_v").
///
/// Eligibility: a field is eligible if the schema's resolved FieldType is a
/// VectorFieldType with metric != METRIC_NONE.  Per-segment dims must agree
/// (or be inferred consistently); a mismatch throws.
///
/// FAISS ids are segment-local value ranks.  Vector columns include rows for
/// deleted docs (deletes are tracked via liveDocs separately), so those vectors
/// land in FAISS too; the query layer filters current liveDocs.
///
/// Single- and multi-valued vector fields are both supported: every value of
/// every doc is added to FAISS, and the query layer maps each FAISS id back to
/// its owning doc (valueRank -> docId).  IVF+PQ scores are approximate; the
/// query layer rescans candidates from the full-precision vector column before
/// ranking and doc collapse.
class VectorIndexBuilder {
public:
  // Aux index name prefix (e.g. "vec.title_v").
  static constexpr std::string_view NAME_PREFIX = "vec.";
  static constexpr std::string_view KIND = "vector_faiss";

  // Working-memory budget for the COSINE renormalization buffer.  Mutable so
  // tests can shrink it to exercise the multi-chunk loop on small inputs.
  static size_t renormChunkBytes;

  // Normal aux builds attempt IVF+PQ.  Tests may disable this to verify the
  // flat-over-column fallback without changing request selectors.
  static bool buildFaissIvfPqAuxIndexes;

  // IVF+PQ tuning defaults.  Zero means "derive from the field / collection".
  static int32_t ivfPqNList;
  static int32_t ivfPqM;
  static int32_t ivfPqBits;
  static int32_t ivfPqDefaultNProbe;
  static int64_t ivfPqMinTrainingVectors;
  static int64_t ivfPqBuildThresholdScanCost;
  static int64_t ivfPqBuildCountForTests;
  static size_t ivfPqTrainingSampleBytes;
  static size_t ivfPqAddChunkBytes;

  /// One segment's worth of input.
  struct SegInput {
    uint64_t segId;
    PostingsReader* postingsReader;
  };

  /// indexGen is the gen the new commit will be published under (for filenames).
  /// Overlay filenames are segment-prefixed (getSegmentOverlayFileName), so
  /// they are unique per (segment, name, gen) with no extra ordinal.  coreGen
  /// is accepted for the old call-site shape but is intentionally not
  /// recorded on vector overlays.
  VectorIndexBuilder(Directory& dir, std::span<const SegInput> segments,
                     const Schema& schema, uint64_t indexGen, uint64_t coreGen)
    : dir_(dir), segments_(segments), schema_(schema),
      indexGen_(indexGen), coreGen_(coreGen) {}

  /// Build aux indexes for vector fields matching `selectors`.
  ///   selectors == ["*"]      - every eligible field
  ///   selectors == ["vec.X"]  - exact match on aux index name
  /// `skipNames` contains overlay names already present on this live segment.
  /// Segment liveness is the validity rule, so those entries are skipped.
  /// Returned AuxIndexInfo entries should be appended to that segment's
  /// SegmentInfo.overlays.  File names of every produced file are appended to
  /// outFilesToSync so the caller can fsync them before publishing IndexInfo.
  std::vector<proto::AuxIndexInfo> build(
      const std::vector<std::string>& selectors,
      const boost::unordered_flat_set<std::string>& skipNames,
      std::vector<std::string>& outFilesToSync);

private:
  Directory& dir_;
  std::span<const SegInput> segments_;
  const Schema& schema_;
  uint64_t indexGen_;
  uint64_t coreGen_;

  // Returns true if any selector matches name.
  static bool selectorMatches(const std::vector<std::string>& selectors, std::string_view name);

  // Produce the full set of concrete vector field names present in *any* segment,
  // resolving each through the schema and keeping only those with metric != NONE.
  // Output map: fieldName -> pinned schema dims (0 == infer from segment).
  std::vector<std::pair<std::string, const VectorFieldType*>> collectEligibleFields();

  // Builds the FAISS index file for one field.  Returns nullopt when
  // no segment has any vectors for the field (no files written, nothing to record).
  std::optional<proto::AuxIndexInfo> buildField(std::string_view fieldName,
                                                const VectorFieldType& ft,
                                                std::vector<std::string>& outFilesToSync);

  std::optional<proto::AuxIndexInfo> buildIvfPqField(std::string_view fieldName,
                                                     const VectorFieldType& ft,
                                                     std::vector<std::string>& outFilesToSync);
};

} // namespace solux
