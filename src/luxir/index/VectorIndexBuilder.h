#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>
#include <boost/unordered/unordered_flat_set.hpp>
#include "luxir/index/AuxInfo.h"
#include "luxir/store/Directory.h"
#include "luxir/schema/Schema.h"
#include "luxir/reader/PostingsReader.h"

namespace luxir {

/// Builds FAISS aux indexes for vector fields in one segment.
///
/// Flat kNN is served directly from the vector column.  Aux builds produce an
/// IVF+PQ ANN index when enough vectors are present and the segment is large
/// enough to be worth indexing.
///
/// One index per (eligible) vector field per segment, written under a single
/// filename produced by Postings::getSegmentOverlayFileName.  The "name"
/// recorded in the segment overlay is "vec.<fieldName>" (e.g. "vec.title_v").
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
  static std::atomic<int64_t> ivfPqBuildCountForTests;
  static std::atomic<int64_t> ivfPqMergeBuildCountForTests;
  static size_t ivfPqTrainingSampleBytes;
  static size_t ivfPqAddChunkBytes;


  enum class BuildSite {
    COMMIT,
    MERGE
  };

  /// One segment's worth of input.
  struct SegInput {
    uint64_t segId;
    PostingsReader* postingsReader;
  };

  /// overlayGen is the per-(segment, field) rebuild ordinal used in filenames
  /// and AuxIndexInfo.gen.  Overlay filenames are segment-prefixed
  /// (getSegmentOverlayFileName), so gen only has to distinguish rebuilds for
  /// the same live segment and field.
  VectorIndexBuilder(Directory& dir, std::span<const SegInput> segments,
                     const Schema& schema, uint64_t overlayGen)
    : dir_(dir), segments_(segments), schema_(schema),
      overlayGen_(overlayGen) {}

  /// Expand selectors to concrete eligible overlay names present in this
  /// builder's segment inputs.  This applies schema and field eligibility but
  /// not the training floor or build threshold.
  std::vector<std::string> matchingOverlayNames(const std::vector<std::string>& selectors);

  /// Build aux indexes for vector fields matching `selectors`.
  ///   selectors == ["*"]      - every eligible field
  ///   selectors == ["vec.X"]  - exact match on aux index name
  /// `skipNames` contains overlay names already present on this live segment.
  /// Segment liveness is the validity rule, so those entries are skipped.
  /// Returned AuxIndexInfo entries should be appended to that segment's
  /// SegmentInfo.overlays.  File names of every produced file are appended to
  /// outFilesToSync so the caller can fsync them before publishing IndexInfo.
  std::vector<AuxInfo> build(
      const std::vector<std::string>& selectors,
      const boost::unordered_flat_set<std::string>& skipNames,
      std::vector<std::string>& outFilesToSync,
      BuildSite buildSite);

private:
  Directory& dir_;
  std::span<const SegInput> segments_;
  const Schema& schema_;
  uint64_t overlayGen_;

  // Returns true if any selector matches name.
  static bool selectorMatches(const std::vector<std::string>& selectors, std::string_view name);

  // Produce the full set of concrete vector field names present in *any* segment,
  // resolving each through the schema and keeping only those with metric != NONE.
  // Output map: fieldName -> pinned schema dims (0 == infer from segment).
  std::vector<std::pair<std::string, const VectorFieldType*>> collectEligibleFields();

  // Builds the FAISS index file for one field.  Returns nullopt when
  // no segment has any vectors for the field (no files written, nothing to record).
  std::optional<AuxInfo> buildField(std::string_view fieldName,
                                                const VectorFieldType& ft,
                                                std::vector<std::string>& outFilesToSync,
                                                BuildSite buildSite);

  std::optional<AuxInfo> buildIvfPqField(std::string_view fieldName,
                                                     const VectorFieldType& ft,
                                                     std::vector<std::string>& outFilesToSync,
                                                     BuildSite buildSite);
};

} // namespace luxir
