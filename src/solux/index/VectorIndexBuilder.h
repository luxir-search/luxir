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

/// Builds FAISS aux indexes for vector fields across a snapshot of segments.
///
/// Flat kNN is served directly from the vector column.  Normal aux builds now
/// produce an IVF+PQ ANN index when enough vectors are present; the old
/// FAISS-flat build remains as an explicit test / A-B benchmark hook.
///
/// One index per (eligible) vector field, written under a single filename
/// produced by Postings::getAuxIndexFileName.  The "name" used in filenames +
/// AuxIndexInfo is "vec.<fieldName>" (e.g. "vec.title_v").
///
/// Eligibility: a field is eligible if the schema's resolved FieldType is a
/// VectorFieldType with metric != METRIC_NONE.  Per-segment dims must agree
/// (or be inferred consistently); a mismatch throws.
///
/// FAISS-id -> (segment, valueRank) is *not* persisted: every value in the
/// vector column of every segment in `segments` (in the order given) is added
/// to FAISS, so at query time the mapping can be derived from each segment's
/// numValues.  Vector columns include rows for deleted docs (deletes are
/// tracked via liveDocs separately), so those vectors land in FAISS too -
/// the query layer is expected to filter against current liveDocs.
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

  // A-B hook: production flat search does not emit a vec.* aux artifact.
  // Tests / benches can enable this to compare FAISS-flat against the normal
  // IVF+PQ aux engine.
  static bool buildFaissFlatAuxIndexes;

  // Normal aux builds attempt IVF+PQ.  Tests may disable this to verify the
  // flat-over-column fallback without changing request selectors.
  static bool buildFaissIvfPqAuxIndexes;

  // IVF+PQ tuning defaults.  Zero means "derive from the field / collection".
  static int32_t ivfPqNList;
  static int32_t ivfPqM;
  static int32_t ivfPqBits;
  static int32_t ivfPqDefaultNProbe;
  static int64_t ivfPqMinTrainingVectors;
  static size_t ivfPqTrainingSampleBytes;
  static size_t ivfPqAddChunkBytes;

  /// One segment's worth of input.  Segments must be passed in the same order
  /// they will appear in the IndexInfo file (sorted by segId), so query-time
  /// FAISS-id -> segment derivation lines up.
  struct SegInput {
    uint64_t segId;
    PostingsReader* postingsReader;
  };

  /// indexGen is the gen the new commit will be published under (for filenames).
  /// coreGen is the segment-composition gen for the new commit; recorded as
  /// AuxIndexInfo.built_core_gen so future commits can invalidate this entry
  /// if segment composition changes.
  VectorIndexBuilder(Directory& dir, std::span<const SegInput> segments,
                     const Schema& schema, uint64_t indexGen, uint64_t coreGen)
    : dir_(dir), segments_(segments), schema_(schema),
      indexGen_(indexGen), coreGen_(coreGen) {}

  /// Build aux indexes for vector fields matching `selectors`.
  ///   selectors == ["*"]      - every eligible field
  ///   selectors == ["vec.X"]  - exact match on aux index name
  /// `skipNames` contains aux names whose previously-built entry is still
  /// valid for this commit (built_core_gen matches current coreGen).  These
  /// are skipped - rebuilding would produce bit-identical output, so the
  /// caller should keep the carried-forward entry instead.
  /// Returned AuxIndexInfo entries should be appended to IndexInfo.aux_indexes
  /// alongside any carried entries.  File names of every produced file are
  /// appended to outFilesToSync so the caller can fsync them before
  /// publishing the new IndexInfo.
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

  std::optional<proto::AuxIndexInfo> buildFlatField(std::string_view fieldName,
                                                    const VectorFieldType& ft,
                                                    std::vector<std::string>& outFilesToSync);

  std::optional<proto::AuxIndexInfo> buildIvfPqField(std::string_view fieldName,
                                                     const VectorFieldType& ft,
                                                     std::vector<std::string>& outFilesToSync);
};

} // namespace solux
