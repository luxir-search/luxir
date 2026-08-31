#pragma once

#include <array>
#include <cstdint>

namespace luxir {

// Skip-effectiveness counters for the internal validation harness (the go/no-go
// for the skip-vs-skip top-k benchmark, and the level2 decision). Deliberately
// non-atomic and single-thread: drive the harness on one thread (force-merged
// single segment is also what the measurement wants) so per-query block
// accounting is deterministic. Concurrent decoders would race these, producing
// approximate totals -- do not read them under intra-query parallelism.
//
// Zero cost in production: every bump is behind the `enabled` gate (default
// false), a branch predicted-not-taken, and the counted events are per-block /
// per-skip, not per-doc.
struct SkipStats {
  static constexpr size_t MAX_SWEEP_LEVELS = 16;

  static inline bool enabled = false;

  // Docs-block body decodes (full 128-doc blocks + the StreamVByte tail). This is
  // the real decode work; blocks_decoded(pruned) / blocks_decoded(exhaustive) is
  // the "% of blocks decoded vs total" headline.
  static inline int64_t docBlocksDecoded = 0;
  // Full 128-value tfreq block decodes. StreamVByte tails are excluded because
  // their docs/freq streams have no independently skippable block boundary.
  static inline int64_t tfreqBlocksDecoded = 0;
  // Per-block L0 headers walked in skipToBlock (within an L1 group). Bounded by
  // ~L1_PERIOD per advance; the within-group skip cost.
  static inline int64_t l0HeaderSteps = 0;
  // Direct jumps to sampled L0 headers within an L1 group.
  static inline int64_t l0CheckpointJumps = 0;
  // L1 group headers walked in skipToBlock. The level2 decision metric: if this
  // dominates on long-list queries, a coarser third level pays off.
  static inline int64_t l1GroupSteps = 0;
  // DocsEnum::advance() and advance-or-begin probe invocations (leapfrog /
  // impact-skip driver calls).
  static inline int64_t advanceCalls = 0;
  // Position-stream accounting: O(1) seeks off the L0 posByteOff anchors
  // (skipToBlock), whole position blocks hopped without unpacking
  // (startPositions), and position block body decodes.
  static inline int64_t posSeeks = 0;
  // Block-max conjunction: bound evaluations vs ranges actually skipped.
  static inline int64_t conjRangeEvals = 0;
  static inline int64_t conjRangeSkips = 0;
  // WAND advance-work savings: clauses retained in the non-essential tail by
  // score, and candidate pivots rejected before being returned to the parent.
  static inline int64_t wandAdvancePrunes = 0;
  static inline int64_t wandCandidatePrunes = 0;
  // Competitive conjunction evaluations whose certified horizon was extended
  // by the adaptive geometric back-off.
  static inline int64_t conjEvalBackoffs = 0;
  static inline int64_t posBlocksSkipped = 0;
  static inline int64_t posBlocksDecoded = 0;
  // Phrase per-candidate outcomes: docs rejected by the pre-position score
  // bound vs docs that paid a position verification.
  static inline int64_t phraseBoundRejects = 0;
  static inline int64_t phraseVerifies = 0;
  // Phrase competitive-block gate: cold certificate recomputations and the
  // per-term impact blocks those lookups proved non-competitive.
  static inline int64_t phraseCompetitiveColdLookups = 0;
  static inline int64_t phraseImpactBlocksSkipped = 0;
  // Shallow-advance outcomes: targets answered from the cached shallow block
  // vs calls that had to move the block cursor (search + possible group parse).
  static inline int64_t shallowCacheHits = 0;
  static inline int64_t shallowCursorMoves = 0;
  static inline int64_t impactGroupBoundCalls = 0;
  static inline int64_t impactGroupBoundNoL0 = 0;
  static inline int64_t impactGroupShallowAnswers = 0;
  static inline int64_t impactRefinesTriggered = 0;
  static inline int64_t impactGroupHeaderParses = 0;
  static inline int64_t impactL0GroupParses = 0;
  static inline int64_t impactL0GroupParseScratchSpills = 0;
  static inline int64_t impactCompetitiveColdLookups = 0;
  static inline int64_t impactCertificateInvalidations = 0;
  static inline int64_t impactCertificateSurvivedRises = 0;
  static inline int64_t maxScoreOuterWindows = 0;
  static inline int64_t maxScoreInnerWindows = 0;
  static inline int64_t maxScoreOuterWindowRefines = 0;
  static inline int64_t maxScoreSetupFallbackBlockBounds = 0;
  static inline int64_t maxScoreSweepWindows = 0;
  static inline int64_t maxScoreRequiredSweeps = 0;
  static inline int64_t maxScoreBufferCompactions = 0;
  static inline int64_t maxScoreDirectFills = 0;
  static inline int64_t maxScoreDeadOuterJumps = 0;
  static inline int64_t maxScoreAnchorJumps = 0;
  static inline int64_t maxScoreTop2Conversions = 0;
  static inline int64_t maxScorePartitionLatchReuses = 0;
  static inline int64_t maxScorePartitionLatchBreaks = 0;
  static inline int64_t maxScoreThresholdRefreshes = 0;
  static inline int64_t maxScoreHalfWindowClips = 0;
  static inline int64_t mandOptWindowSkips = 0;
  static inline int64_t mandOptConjunctionWindows = 0;
  static inline int64_t mandOptWindowEvals = 0;
  static inline int64_t mandOptOptionalVerifies = 0;
  static inline int64_t mandOptBulkWindows = 0;
  static inline int64_t mandOptBulkWindowSkips = 0;
  static inline int64_t mandOptBulkSweeps = 0;
  static inline int64_t mandOptBulkCompactions = 0;
  static inline int64_t mandOptBulkOptDrivenWindows = 0;
  static inline int64_t mandOptBulkScalarFillFallbacks = 0;
  static inline int64_t countBulkFillCalls = 0;
  static inline int64_t countBulkFillBlocks = 0;
  static inline int64_t countBulkFillDocs = 0;
  static inline int64_t countBulkFillContiguousBlocks = 0;
  // Whole word-encoded (bitset/contiguous) doc blocks OR'd into count windows
  // straight from the stream, without decoding to the doc buffer.
  static inline int64_t countBulkFillWordBlocks = 0;
  static inline int64_t conjDenseCountWindows = 0;
  static inline int64_t negatedCountWindows = 0;
  static inline int64_t negatedCountExclFills = 0;
  static inline int64_t bulkExclusionEngagements = 0;
  static inline int64_t bulkExclusionWindows = 0;
  static inline int64_t bulkExclusionFills = 0;
  static inline int64_t bulkExclusionDisabledFallbacks = 0;
  static inline int64_t bulkExclusionShapeFallbacks = 0;
  static inline int64_t bulkExclusionPositiveSegmentFallbacks = 0;
  static inline int64_t bulkExclusionUnsupportedFallbacks = 0;
  static inline int64_t phraseExclusionWindowAdmits = 0;
  static inline int64_t phraseExclusionWindowRejects = 0;
  static inline int64_t prohibitedCachePullHits = 0;
  static inline int64_t prohibitedCachePullBuilds = 0;
  static inline int64_t prohibitedCachePullBypasses = 0;
  static inline int64_t prohibitedCachePullRoutingBypasses = 0;
  static inline int64_t prohibitedCacheDenseCountHits = 0;
  static inline int64_t prohibitedCacheDenseCountBuilds = 0;
  static inline int64_t prohibitedCacheDenseCountBypasses = 0;
  static inline int64_t prohibitedCacheDenseCountRoutingBypasses = 0;
  static inline int64_t prohibitedCacheMaxScoreHits = 0;
  static inline int64_t prohibitedCacheMaxScoreBuilds = 0;
  static inline int64_t prohibitedCacheMaxScoreBypasses = 0;
  static inline int64_t prohibitedCacheMaxScoreRoutingBypasses = 0;
  static inline int64_t prohibitedCacheCandidateHits = 0;
  static inline int64_t prohibitedCacheCandidateBuilds = 0;
  static inline int64_t prohibitedCacheCandidateBypasses = 0;
  static inline int64_t prohibitedCacheCandidateRoutingBypasses = 0;
  static inline int64_t conjDenseMatchWindows = 0;
  static inline int64_t conjDenseScoredWindows = 0;
  static inline int64_t conjDenseScoredAdmits = 0;
  static inline int64_t conjDenseScoredLatchBacks = 0;
  static inline int64_t conjDenseScoredDensityRejects = 0;
  static inline int64_t conjDenseScoredCostRejects = 0;
  static inline int64_t conjDenseScoredFilterRejects = 0;
  static inline int64_t conjExactScoredBoundsBypasses = 0;
  static inline int64_t conjExactDirectApproxEngagements = 0;
  static inline int64_t conjDirectDenseEngagements = 0;
  static inline int64_t conjDisjGroupCountWindows = 0;
  static inline int64_t conjDisjGroupScoreWindows = 0;
  // Pure disjunction count windows containing exact all-term conjunction members.
  static inline int64_t disjConjGroupCountWindows = 0;
  static inline int64_t disjCountIdentityEngagements = 0;
  static inline int64_t disjCountIdentityDeleteFallbacks = 0;
  static inline int64_t disjCountIdentityFilterFallbacks = 0;
  static inline int64_t disjCountIdentityDomainOutputFallbacks = 0;
  static inline int64_t disjCountIdentityNonTermFallbacks = 0;
  static inline int64_t disjCountIdentityMinMatchFallbacks = 0;
  static inline int64_t disjCountIdentityRequiredFallbacks = 0;
  static inline int64_t disjCountIdentityProhibitedFallbacks = 0;
  static inline int64_t disjCountIdentityProfitabilityFallbacks = 0;
  static inline int64_t filteredDisjBatchEngagements = 0;
  static inline int64_t filteredDisjBatchPostingsFeedEngagements = 0;
  static inline int64_t filteredDisjBatchCountWindows = 0;
  static inline int64_t filteredDisjBatchScoreWindows = 0;
  static inline int64_t filteredDisjBatchMatchWindows = 0;
  static inline int64_t filteredDisjBatchDensityFallbacks = 0;
  static inline int64_t filteredDisjBatchUnsupportedFilterFeedFallbacks = 0;
  static inline int64_t filteredDisjBatchNonTermFallbacks = 0;
  static inline int64_t filteredConjBatchEngagements = 0;
  static inline int64_t filteredConjBatchMultiTermEngagements = 0;
  static inline int64_t filteredConjBatchPostingsFeedEngagements = 0;
  static inline int64_t filteredConjBatchCountWindows = 0;
  static inline int64_t filteredConjBatchScoreWindows = 0;
  static inline int64_t filteredCountCandidateAdmits = 0;
  static inline int64_t filteredCountCandidateDenseLatchBacks = 0;
  static inline int64_t exactTermCountEngagements = 0;
  static inline int64_t exactTermCountBatches = 0;
  static inline int64_t candidateTermFeedEngagements = 0;
  static inline int64_t conjTermLeadFirstFillLeapfrogs = 0;
  static inline int64_t exactFilteredMandOptCompositions = 0;
  static inline int64_t exactCountTopKCompositions = 0;
  static inline int64_t exactCountTopKProfitabilityRejects = 0;
  static inline int64_t exactCountTopKSparseFilterSinglePassRejects = 0;
  static inline int64_t exactCountTopKBulkFallbacks = 0;
  static inline int64_t bulkBuiltThenRejected = 0;
  static inline int64_t bulkBuiltThenRejectedWrapperRoute = 0;
  static inline int64_t bulkBuiltThenRejectedMandOptTwoPhase = 0;
  static inline int64_t bulkBuiltThenRejectedMaxScoreProhibited = 0;
  static inline int64_t bulkBuiltThenRejectedFilterAttach = 0;
  static inline int64_t bulkBuiltThenRejectedFilteredDisj = 0;
  static inline int64_t bulkBuiltThenRejectedExactMandOpt = 0;
  static inline int64_t bulkBuiltThenRejectedFilteredScored = 0;
  static inline int64_t bulkBuiltThenRejectedFilterOnly = 0;
  static inline int64_t bulkBuiltThenRejectedSortMatchWindow = 0;
  static inline int64_t bulkBuiltThenRejectedExactComposition = 0;
  static inline int64_t conjPlanUnknownIsland = 0;
  static inline int64_t conjPlanUnknownIslandMultiTerm = 0;
  static inline int64_t conjPlanUnknownIslandPhrase = 0;
  static inline int64_t conjPlanUnknownIslandNumericGeo = 0;
  static inline int64_t conjPlanUnknownIslandOther = 0;
  static inline int64_t multitermExpansions = 0;
  static inline int64_t sparseFilteredTopKReroutes = 0;
  static inline int64_t sparseFilteredTopKUnionReroutes = 0;
  static inline int64_t sparseFilteredTopKDensityRejects = 0;
  static inline int64_t sparseFilteredTopKShapeRejects = 0;
  static inline int64_t selectionRefinersElided = 0;
  static inline int64_t selectionValueFiltersPlanned = 0;
  static inline int64_t selectionLogicalFiltersPlanned = 0;
  static inline int64_t selectionUnionCompositions = 0;
  static inline int64_t ownedFilterMaterializations = 0;
  static inline int64_t ownedFilterServes = 0;
  static inline int64_t conjCountFallbacks = 0;
  static inline int64_t conjMatchFallbacks = 0;
  static inline int64_t bulkDomainWindowsFed = 0;
  static inline int64_t constantPullDomainCollections = 0;
  static inline int64_t constantWindowCaptures = 0;
  static inline int64_t fieldSortBlocksSkipped = 0;
  static inline int64_t fieldSortCandidateActivations = 0;
  static inline int64_t fieldSortCandidateTerminations = 0;
  static inline int64_t fieldSortBulkCollections = 0;
  // Field-sort pruning attribution: ranges issued before the heap filled
  // (the warmup transient, in key blocks) and competitive ranges issued after
  // (the visited-block count the bound rules could not skip).
  static inline int64_t fieldSortWarmupRanges = 0;
  static inline int64_t fieldSortCompetitiveRanges = 0;
  // External two-phase field-sort pull driver: segment activations,
  // approximation conjunction landings in competitive ranges, candidates
  // rejected by the outer domain, exact verification calls, and candidates
  // rejected by the live heap bottom before exact verification.
  static inline int64_t fieldSortExternalApproxActivations = 0;
  static inline int64_t fieldSortExternalApproxCandidates = 0;
  static inline int64_t fieldSortExternalApproxDomainRejects = 0;
  static inline int64_t fieldSortExternalApproxVerifications = 0;
  static inline int64_t fieldSortExternalApproxBoundRejects = 0;
  // Docs pushed through the bulk key-gather path, and the gathered-doc index
  // of the last heap change - "how late did the top-k stop moving".
  static inline int64_t fieldSortDocsGathered = 0;
  static inline int64_t fieldSortGatherAtLastAdmission = 0;
  // Blocks whose best key strictly undercuts the bottom: the visit floor no
  // traversal order can avoid (bounds cover all docs, so a block holding a
  // better-bounded key must be inspected to refute it). SEGMENT-LOCAL:
  // recorded at each segment's completion against the then-current bottom,
  // so multi-segment sums overstate the final-bottom floor - single-segment
  // runs give the honest reading.
  static inline int64_t fieldSortIrreducibleBlocks = 0;
  // Equality-aware operational floor: blocks a fresh traversal would still
  // collect under the full tie/segdoc rules. Segment-local, as above.
  static inline int64_t fieldSortRequiredBlocks = 0;
  // Leaf-level twins of the three floor/skip counters above (512-value
  // units; never compare against the 4096-unit coarse counters directly).
  static inline int64_t fieldSortLeavesSkipped = 0;
  static inline int64_t fieldSortIrreducibleLeaves = 0;
  static inline int64_t fieldSortRequiredLeaves = 0;
  // Best-first exact-domain driver: route activations, coarse nodes
  // expanded into leaves, leaves gathered, proof terminations (heap head
  // strictly noncompetitive), and work-cap fallbacks to the forward sweep.
  static inline int64_t fieldSortBestFirstActivations = 0;
  static inline int64_t fieldSortBestFirstExpansions = 0;
  static inline int64_t fieldSortBestFirstLeaves = 0;
  static inline int64_t fieldSortBestFirstTerminations = 0;
  static inline int64_t fieldSortBestFirstFallbacks = 0;
  // Seeded two-pass query-driven driver: route activations, seed leaves the
  // scorer actually enumerated, seeds the maturing bottom classified away
  // before any postings work, seed-schedule aborts (underfilled heap or a
  // matchless run), and pass-2 jumps over already-enumerated seed leaves.
  static inline int64_t fieldSortSeededActivations = 0;
  static inline int64_t fieldSortSeedLeaves = 0;
  static inline int64_t fieldSortSeedClassifiedOut = 0;
  static inline int64_t fieldSortSeedFillAborts = 0;
  static inline int64_t fieldSortSeedPass2Skips = 0;
  static inline int64_t filterDocSetIdentityCollections = 0;
  static inline int64_t exactDomainDocSetCollections = 0;
  static inline int64_t exactDomainStreamFallbacks = 0;
  static inline int64_t wholeCountHits = 0;
  static inline int64_t wholeCountBuilds = 0;
  static inline int64_t wholeCountBypasses = 0;
  static inline int64_t wholeCountConstant = 0;
  static inline int64_t wholeCountFallbackSuppliers = 0;
  static inline int64_t cacheFirstMembershipWeightSkips = 0;
  static inline int64_t cacheFirstExactDomainWeightSkips = 0;
  static inline int64_t cacheFirstTopKCountWeightSkips = 0;
  static inline int64_t cacheFirstConstantTopKWeightSkips = 0;
  static inline int64_t cacheFirstTopKRankingWeights = 0;
  static inline int64_t cacheFirstFieldSortWeightSkips = 0;
  static inline int64_t queryContextsCreated = 0;
  static inline int64_t cacheFirstQueryContextsOmitted = 0;
  static inline int64_t wholeTopKCountHits = 0;
  static inline int64_t wholeTopKCountBuilds = 0;
  static inline int64_t wholeTopKCountBypasses = 0;
  static inline int64_t wholeTopKCountConstant = 0;
  static inline int64_t wholeTopKCountFallbackSuppliers = 0;
  static inline int64_t wholeFieldSortHits = 0;
  static inline int64_t wholeFieldSortBuilds = 0;
  static inline int64_t wholeFieldSortBypasses = 0;
  static inline int64_t wholeFieldSortRoutingBypasses = 0;
  static inline int64_t wholeFieldSortConstant = 0;
  static inline int64_t wholeFieldSortFallbackSuppliers = 0;
  static inline int64_t wholeFieldSortBestFirstActivations = 0;
  static inline int64_t wholeFieldSortLadderFallbacks = 0;
  static inline int64_t numericPredicateSparseVerifyArms = 0;
  static inline int64_t numericPredicateComplementArms = 0;
  static inline int64_t numericPredicatePointsArms = 0;
  static inline int64_t numericPredicateZoneArms = 0;
  static inline int64_t rangeFacetPointsArms = 0;
  static inline int64_t geoBKDArms = 0;
  static inline int64_t geoScanArms = 0;
  static inline int64_t geoSparseVerifyArms = 0;
  // Codec skipBlock calls on landing-block traversal only: header-level
  // leaps (skipToBlock over whole blocks) bypass freq planes by byte length
  // without charging this counter, so it undercounts under jumped windows.
  static inline int64_t docsOnlyFreqBlocksSkipped = 0;
  static inline int64_t docsOnlyWordProbeAdvances = 0;
  static inline int64_t scoredWordProbeAdvances = 0;
  // Scored resident-probe accounting. Advances count full blocks entered in
  // packed, contiguous, or word form. A survivor block is counted once, on its
  // first termFreq() request after scoring survival; its freq block may then be
  // decoded once lazily.
  static inline int64_t scoredProbeAdvances = 0;
  static inline int64_t scoredProbeSurvivorBlocks = 0;
  static inline int64_t scoredProbeFreqDecodes = 0;
  static inline int64_t scoredProbeWordExpansions = 0;
  // Full tfreq blocks decoded while producing the lead candidate batches in a
  // score-first conjunction. This lets the ownership-boundary invariant
  // distinguish unavoidable lead work from non-lead probe work.
  static inline int64_t conjScoredLeadFreqDecodes = 0;
  // MaxScore non-essential sweep economics. Counter writes are aggregate-only:
  // candidate-sized batches are added at call boundaries, never from the
  // per-candidate loops. The generic applyToCandidates totals let the
  // measurement harness reconcile the per-level MaxScore accounting.
  static inline int64_t applyToCandidatesCalls = 0;
  static inline int64_t applyToCandidatesCandidates = 0;
  static inline int64_t applyToCandidatesAdvances = 0;
  static inline int64_t applyToCandidatesMatches = 0;
  static inline int64_t applyToCandidatesWordProbeBegins = 0;
  static inline int64_t applyToCandidatesPlainAdvanceFallbacks = 0;
  static inline int64_t maxScoreSweepCalls = 0;
  static inline int64_t maxScoreSweepEntryCandidates = 0;
  static inline int64_t maxScoreProbeCandidates = 0;
  static inline int64_t maxScoreRequiredProbeCandidates = 0;
  static inline int64_t maxScoreOptionalProbeCandidates = 0;
  static inline int64_t maxScoreRequiredMatches = 0;
  static inline int64_t maxScoreOptionalMatches = 0;
  static inline int64_t maxScoreCompactionInput = 0;
  static inline int64_t maxScoreCompactionKept = 0;
  static inline int64_t maxScoreFinalCompactionInput = 0;
  static inline int64_t maxScoreFinalCompactionKept = 0;
  static inline std::array<int64_t, MAX_SWEEP_LEVELS> maxScoreProbeCandidatesByLevel{};
  static inline std::array<int64_t, MAX_SWEEP_LEVELS> maxScoreAdvancesByLevel{};
  static inline std::array<int64_t, MAX_SWEEP_LEVELS> maxScoreMatchesByLevel{};
  static inline std::array<int64_t, MAX_SWEEP_LEVELS> maxScoreCompactionInputByLevel{};
  static inline std::array<int64_t, MAX_SWEEP_LEVELS> maxScoreCompactionKeptByLevel{};

  static void reset() {
    docBlocksDecoded = 0;
    tfreqBlocksDecoded = 0;
    l0HeaderSteps = 0;
    l0CheckpointJumps = 0;
    l1GroupSteps = 0;
    advanceCalls = 0;
    posSeeks = 0;
    conjRangeEvals = 0;
    conjRangeSkips = 0;
    wandAdvancePrunes = 0;
    wandCandidatePrunes = 0;
    conjEvalBackoffs = 0;
    posBlocksSkipped = 0;
    posBlocksDecoded = 0;
    phraseBoundRejects = 0;
    phraseVerifies = 0;
    phraseCompetitiveColdLookups = 0;
    phraseImpactBlocksSkipped = 0;
    shallowCacheHits = 0;
    shallowCursorMoves = 0;
    impactGroupBoundCalls = 0;
    impactGroupBoundNoL0 = 0;
    impactGroupShallowAnswers = 0;
    impactRefinesTriggered = 0;
    impactGroupHeaderParses = 0;
    impactL0GroupParses = 0;
    impactL0GroupParseScratchSpills = 0;
    impactCompetitiveColdLookups = 0;
    impactCertificateInvalidations = 0;
    impactCertificateSurvivedRises = 0;
    maxScoreOuterWindows = 0;
    maxScoreInnerWindows = 0;
    maxScoreOuterWindowRefines = 0;
    maxScoreSetupFallbackBlockBounds = 0;
    maxScoreSweepWindows = 0;
    maxScoreRequiredSweeps = 0;
    maxScoreBufferCompactions = 0;
    maxScoreDirectFills = 0;
    maxScoreDeadOuterJumps = 0;
    maxScoreAnchorJumps = 0;
    maxScoreTop2Conversions = 0;
    maxScorePartitionLatchReuses = 0;
    maxScorePartitionLatchBreaks = 0;
    maxScoreThresholdRefreshes = 0;
    maxScoreHalfWindowClips = 0;
    mandOptWindowSkips = 0;
    mandOptConjunctionWindows = 0;
    mandOptWindowEvals = 0;
    mandOptOptionalVerifies = 0;
    mandOptBulkWindows = 0;
    mandOptBulkWindowSkips = 0;
    mandOptBulkSweeps = 0;
    mandOptBulkCompactions = 0;
    mandOptBulkOptDrivenWindows = 0;
    mandOptBulkScalarFillFallbacks = 0;
    countBulkFillCalls = 0;
    countBulkFillBlocks = 0;
    countBulkFillDocs = 0;
    countBulkFillContiguousBlocks = 0;
    countBulkFillWordBlocks = 0;
    conjDenseCountWindows = 0;
    negatedCountWindows = 0;
    negatedCountExclFills = 0;
    bulkExclusionEngagements = 0;
    bulkExclusionWindows = 0;
    bulkExclusionFills = 0;
    bulkExclusionDisabledFallbacks = 0;
    bulkExclusionShapeFallbacks = 0;
    bulkExclusionPositiveSegmentFallbacks = 0;
    bulkExclusionUnsupportedFallbacks = 0;
    phraseExclusionWindowAdmits = 0;
    phraseExclusionWindowRejects = 0;
    prohibitedCachePullHits = 0;
    prohibitedCachePullBuilds = 0;
    prohibitedCachePullBypasses = 0;
    prohibitedCachePullRoutingBypasses = 0;
    prohibitedCacheDenseCountHits = 0;
    prohibitedCacheDenseCountBuilds = 0;
    prohibitedCacheDenseCountBypasses = 0;
    prohibitedCacheDenseCountRoutingBypasses = 0;
    prohibitedCacheMaxScoreHits = 0;
    prohibitedCacheMaxScoreBuilds = 0;
    prohibitedCacheMaxScoreBypasses = 0;
    prohibitedCacheMaxScoreRoutingBypasses = 0;
    prohibitedCacheCandidateHits = 0;
    prohibitedCacheCandidateBuilds = 0;
    prohibitedCacheCandidateBypasses = 0;
    prohibitedCacheCandidateRoutingBypasses = 0;
    conjDenseMatchWindows = 0;
    conjDenseScoredWindows = 0;
    conjDenseScoredAdmits = 0;
    conjDenseScoredLatchBacks = 0;
    conjDenseScoredDensityRejects = 0;
    conjDenseScoredCostRejects = 0;
    conjDenseScoredFilterRejects = 0;
    conjExactScoredBoundsBypasses = 0;
    conjExactDirectApproxEngagements = 0;
    conjDirectDenseEngagements = 0;
    conjDisjGroupCountWindows = 0;
    conjDisjGroupScoreWindows = 0;
    disjConjGroupCountWindows = 0;
    disjCountIdentityEngagements = 0;
    disjCountIdentityDeleteFallbacks = 0;
    disjCountIdentityFilterFallbacks = 0;
    disjCountIdentityDomainOutputFallbacks = 0;
    disjCountIdentityNonTermFallbacks = 0;
    disjCountIdentityMinMatchFallbacks = 0;
    disjCountIdentityRequiredFallbacks = 0;
    disjCountIdentityProhibitedFallbacks = 0;
    disjCountIdentityProfitabilityFallbacks = 0;
    filteredDisjBatchEngagements = 0;
    filteredDisjBatchPostingsFeedEngagements = 0;
    filteredDisjBatchCountWindows = 0;
    filteredDisjBatchScoreWindows = 0;
    filteredDisjBatchMatchWindows = 0;
    filteredDisjBatchDensityFallbacks = 0;
    filteredDisjBatchUnsupportedFilterFeedFallbacks = 0;
    filteredDisjBatchNonTermFallbacks = 0;
    filteredConjBatchEngagements = 0;
    filteredConjBatchMultiTermEngagements = 0;
    filteredConjBatchPostingsFeedEngagements = 0;
    filteredConjBatchCountWindows = 0;
    filteredConjBatchScoreWindows = 0;
    filteredCountCandidateAdmits = 0;
    filteredCountCandidateDenseLatchBacks = 0;
    exactTermCountEngagements = 0;
    exactTermCountBatches = 0;
    candidateTermFeedEngagements = 0;
    conjTermLeadFirstFillLeapfrogs = 0;
    exactFilteredMandOptCompositions = 0;
    exactCountTopKCompositions = 0;
    exactCountTopKProfitabilityRejects = 0;
    exactCountTopKSparseFilterSinglePassRejects = 0;
    exactCountTopKBulkFallbacks = 0;
    bulkBuiltThenRejected = 0;
    bulkBuiltThenRejectedWrapperRoute = 0;
    bulkBuiltThenRejectedMandOptTwoPhase = 0;
    bulkBuiltThenRejectedMaxScoreProhibited = 0;
    bulkBuiltThenRejectedFilterAttach = 0;
    bulkBuiltThenRejectedFilteredDisj = 0;
    bulkBuiltThenRejectedExactMandOpt = 0;
    bulkBuiltThenRejectedFilteredScored = 0;
    bulkBuiltThenRejectedFilterOnly = 0;
    bulkBuiltThenRejectedSortMatchWindow = 0;
    bulkBuiltThenRejectedExactComposition = 0;
    conjPlanUnknownIsland = 0;
    conjPlanUnknownIslandMultiTerm = 0;
    conjPlanUnknownIslandPhrase = 0;
    conjPlanUnknownIslandNumericGeo = 0;
    conjPlanUnknownIslandOther = 0;
    multitermExpansions = 0;
    sparseFilteredTopKReroutes = 0;
    sparseFilteredTopKUnionReroutes = 0;
    sparseFilteredTopKDensityRejects = 0;
    sparseFilteredTopKShapeRejects = 0;
    selectionRefinersElided = 0;
    selectionValueFiltersPlanned = 0;
    selectionLogicalFiltersPlanned = 0;
    selectionUnionCompositions = 0;
    ownedFilterMaterializations = 0;
    ownedFilterServes = 0;
    conjCountFallbacks = 0;
    conjMatchFallbacks = 0;
    bulkDomainWindowsFed = 0;
    constantPullDomainCollections = 0;
    constantWindowCaptures = 0;
    fieldSortBlocksSkipped = 0;
    fieldSortCandidateActivations = 0;
    fieldSortCandidateTerminations = 0;
    fieldSortBulkCollections = 0;
    fieldSortWarmupRanges = 0;
    fieldSortCompetitiveRanges = 0;
    fieldSortExternalApproxActivations = 0;
    fieldSortExternalApproxCandidates = 0;
    fieldSortExternalApproxDomainRejects = 0;
    fieldSortExternalApproxVerifications = 0;
    fieldSortExternalApproxBoundRejects = 0;
    fieldSortDocsGathered = 0;
    fieldSortGatherAtLastAdmission = 0;
    fieldSortIrreducibleBlocks = 0;
    fieldSortRequiredBlocks = 0;
    fieldSortLeavesSkipped = 0;
    fieldSortIrreducibleLeaves = 0;
    fieldSortRequiredLeaves = 0;
    fieldSortBestFirstActivations = 0;
    fieldSortBestFirstExpansions = 0;
    fieldSortBestFirstLeaves = 0;
    fieldSortBestFirstTerminations = 0;
    fieldSortBestFirstFallbacks = 0;
    fieldSortSeededActivations = 0;
    fieldSortSeedLeaves = 0;
    fieldSortSeedClassifiedOut = 0;
    fieldSortSeedFillAborts = 0;
    fieldSortSeedPass2Skips = 0;
    filterDocSetIdentityCollections = 0;
    exactDomainDocSetCollections = 0;
    exactDomainStreamFallbacks = 0;
    wholeCountHits = 0;
    wholeCountBuilds = 0;
    wholeCountBypasses = 0;
    wholeCountConstant = 0;
    wholeCountFallbackSuppliers = 0;
    cacheFirstMembershipWeightSkips = 0;
    cacheFirstExactDomainWeightSkips = 0;
    cacheFirstTopKCountWeightSkips = 0;
    cacheFirstConstantTopKWeightSkips = 0;
    cacheFirstTopKRankingWeights = 0;
    cacheFirstFieldSortWeightSkips = 0;
    queryContextsCreated = 0;
    cacheFirstQueryContextsOmitted = 0;
    wholeTopKCountHits = 0;
    wholeTopKCountBuilds = 0;
    wholeTopKCountBypasses = 0;
    wholeTopKCountConstant = 0;
    wholeTopKCountFallbackSuppliers = 0;
    wholeFieldSortHits = 0;
    wholeFieldSortBuilds = 0;
    wholeFieldSortBypasses = 0;
    wholeFieldSortRoutingBypasses = 0;
    wholeFieldSortConstant = 0;
    wholeFieldSortFallbackSuppliers = 0;
    wholeFieldSortBestFirstActivations = 0;
    wholeFieldSortLadderFallbacks = 0;
    numericPredicateSparseVerifyArms = 0;
    numericPredicateComplementArms = 0;
    numericPredicatePointsArms = 0;
    numericPredicateZoneArms = 0;
    rangeFacetPointsArms = 0;
    geoBKDArms = 0;
    geoScanArms = 0;
    geoSparseVerifyArms = 0;
    docsOnlyFreqBlocksSkipped = 0;
    docsOnlyWordProbeAdvances = 0;
    scoredWordProbeAdvances = 0;
    scoredProbeAdvances = 0;
    scoredProbeSurvivorBlocks = 0;
    scoredProbeFreqDecodes = 0;
    scoredProbeWordExpansions = 0;
    conjScoredLeadFreqDecodes = 0;
    applyToCandidatesCalls = 0;
    applyToCandidatesCandidates = 0;
    applyToCandidatesAdvances = 0;
    applyToCandidatesMatches = 0;
    applyToCandidatesWordProbeBegins = 0;
    applyToCandidatesPlainAdvanceFallbacks = 0;
    maxScoreSweepCalls = 0;
    maxScoreSweepEntryCandidates = 0;
    maxScoreProbeCandidates = 0;
    maxScoreRequiredProbeCandidates = 0;
    maxScoreOptionalProbeCandidates = 0;
    maxScoreRequiredMatches = 0;
    maxScoreOptionalMatches = 0;
    maxScoreCompactionInput = 0;
    maxScoreCompactionKept = 0;
    maxScoreFinalCompactionInput = 0;
    maxScoreFinalCompactionKept = 0;
    maxScoreProbeCandidatesByLevel.fill(0);
    maxScoreAdvancesByLevel.fill(0);
    maxScoreMatchesByLevel.fill(0);
    maxScoreCompactionInputByLevel.fill(0);
    maxScoreCompactionKeptByLevel.fill(0);
  }
};

inline void skipCount(int64_t& counter) {
  if (SkipStats::enabled) {
    counter++;
  }
}

// RAII enable+reset for a measurement scope; restores the previous enabled
// state (single-threaded harness use only, like the counters themselves).
class SkipStatsScope {
  bool saved;

public:
  SkipStatsScope() : saved(SkipStats::enabled) {
    SkipStats::enabled = true;
    SkipStats::reset();
  }
  ~SkipStatsScope() { SkipStats::enabled = saved; }
};

} // namespace luxir
