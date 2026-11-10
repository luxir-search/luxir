// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>
#include <boost/unordered/unordered_flat_set.hpp>
#include <faiss/MetricType.h>
#include "luxir/index/AuxInfo.h"
#include "luxir/store/Directory.h"
#include "luxir/schema/Schema.h"
#include "luxir/reader/PostingsReader.h"

namespace luxir {

class IndexRamBudget;

/// Builds FAISS aux indexes for vector fields in one segment.
///
/// Flat kNN is served directly from the vector column.  Aux builds produce an
/// IVF ANN index (the field's "ann" codec: IVF+RaBitQ by default, or explicit IVF+PQ)
/// when enough vectors are present and the segment is large enough to be
/// worth indexing.
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

  // Normal aux builds attempt an IVF index.  Tests may disable this to verify
  // the flat-over-column fallback without changing request selectors.
  static bool buildFaissAuxIndexes;

  // Build policy.  A segment gets an IVF overlay only when it has at least
  // the codec's training floor of vectors and N * dims >= the scan-cost
  // threshold; smaller segments are scanned exactly from the column.
  static int64_t ivfPqMinTrainingVectors;
  static int64_t ivfRaBitQMinTrainingVectors;
  static int64_t ivfBuildThresholdScanCost;
  static std::atomic<int64_t> ivfBuildCountForTests;
  static std::atomic<int64_t> ivfMergeBuildCountForTests;
  // Training sample size: this many vectors per trained centroid (IVF lists,
  // or the PQ codebook entries when those are more), capped by the segment and
  // by the memory ceiling for the sample's in-memory copy. The codec training
  // floor still applies.
  static int64_t ivfTrainingPointsPerCentroid;
  static size_t ivfTrainingSampleMaxBytes;
  static std::atomic<int64_t> ivfLastTrainingVectorsForTests;
  // Add chunk input-byte target. Chunks also hold at least four row tasks
  // per arena compute slot, capped by available vectors.
  static size_t ivfAddChunkBytes;
  // Training coarse assignment, training rotation and the complete add
  // pipeline split rows across TBB tasks. 0 derives the row floor from
  // ceil(FAISS distance_compute_blas_threshold / dims) to retain BLAS.
  // PQ codebooks train independently per sub-quantizer regardless of this floor.
  static int64_t ivfBuildMinRowsPerTask;

  // Completes a RaBitQ code FAISS encoded for x against its list centroid:
  // corrects the sign-bit factors of near-centroid vectors, and returns the
  // amplitude stored beside the code. x and centroid are in the rotated space.
  static float finishRaBitQCode(const float* x, const float* centroid, uint8_t* code,
                                int32_t dims, int32_t bits, faiss::MetricType metric);

  // Seed of the RaBitQ random rotation.  Fixed so every overlay of a given
  // dims shares one transform (deduplicated in RAM by internVectorRotation).
  static constexpr int RABITQ_ROTATION_SEED = 0x5ab17;


  enum class BuildSite {
    COMMIT,
    MERGE
  };

  /// One segment's worth of input.
  struct SegInput {
    uint64_t segId;
    PostingsReader* postingsReader;
    uint64_t creatorEpoch;

    SegInput(uint64_t segId, PostingsReader* postingsReader, uint64_t creatorEpoch)
      : segId(segId), postingsReader(postingsReader), creatorEpoch(creatorEpoch) {}
  };

  /// overlayGen is the per-(segment, field) rebuild ordinal used in filenames
  /// and AuxIndexInfo.gen.  Overlay filenames are segment-prefixed
  /// (getSegmentOverlayFileName), so gen only has to distinguish rebuilds for
  /// the same live segment and field.
  VectorIndexBuilder(Directory& dir, std::span<const SegInput> segments,
                     const Schema& schema, uint64_t overlayGen, uint64_t writerEpoch,
                     IndexRamBudget* indexRamBudget = nullptr)
    : dir_(dir), segments_(segments), schema_(schema),
      overlayGen_(overlayGen), writerEpoch_(writerEpoch), indexRamBudget(indexRamBudget) {}

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
  uint64_t writerEpoch_;
  IndexRamBudget* indexRamBudget;

  // Returns true if any selector matches name.
  static bool selectorMatches(const std::vector<std::string>& selectors, std::string_view name);

  // Produce the full set of concrete vector field names present in *any* segment,
  // resolving each through the schema and keeping only those with metric != NONE.
  // Output map: fieldName -> pinned schema dims (0 == infer from segment).
  std::vector<std::pair<std::string, const VectorFieldType*>> collectEligibleFields();

  // Builds the FAISS index file for one field.  Returns nullopt when
  // no segment has any vectors for the field (no files written, nothing to record).
  std::optional<AuxInfo> buildIvfField(std::string_view fieldName,
                                      const VectorFieldType& ft,
                                      std::vector<std::string>& outFilesToSync,
                                      BuildSite buildSite);
};

} // namespace luxir
