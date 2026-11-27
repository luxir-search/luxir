// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <deque>
#include <span>
#include <string>
#include <vector>
#include "luxir/api/luxir_types.hpp"
#include "CommitSnapshot.h"
#include "Inverter.h"
#include "luxir/server/LuxirError.h"


namespace luxir {

class IndexWriter;
class UpdateMessage;

// CommitInfo is used to track information about a commit.  This is internally used by the IndexWriter.
// Each commit message owns the coordination state for its flushes and merge waits.
// This really belongs internally to the IndexWriter, but since UpdateMessage isn't scoped within IndexWriter,
// this can't be either (can't forward declare IndexWriter::CommitInfo).
class CommitInfo {
public:
  UpdateMessage* updateMessage = nullptr;  // The update message that triggered this commit.

  // highest update version in this commit, including deletes and the commit message itself.
  uint64_t highestUpdateVersion = 0;
  // The segment-composition generation this commit will be published under.
  // Assigned in finishCommitBody.  Equal to the previous
  // commit's core_gen if segment composition is unchanged, else previous + 1.
  uint64_t coreGen = 0;
  // Number of segments left to flush, protected by same mutex that protects the inverter lists.
  // making this an atomic is not enough to avoid race conditions since we also depend on coordination with
  // inverter->updateMessage, among other things.
  uint32_t leftToFlush = 0;

};

// An update message to be processed by the TBB update flow graph.
// See ProtoUpdateMessage.h/cpp for protobuf update handling code
class UpdateMessage {
public:
  virtual ~UpdateMessage() {}

  // For now, we will allow the handler to obtain/release an inverter.  We could also optionally pass it
  // as a param in the future if obtain/release becomes more complex.
  virtual void handle(IndexWriter& iw) = 0;

  // Called after all operations are complete.  Would typically delete this instance if it was heap allocated.
  // Consumers of UpdateMessage will not touch it after this call.
  virtual void done(IndexWriter& iw) = 0;

  // Internal commit-kind enum.  The proto API exposes commit-or-not via presence of
  // luxir::api::CommitParams; SILENT_COMMIT and CONSISTENT_COMMIT are placeholders for future
  // wiring (no internal branches act on them yet).
  enum CommitType {
    NO_COMMIT = 0,         // the default
    COMMIT = 1,            // ensure new data is searchable
    SILENT_COMMIT = 2,     // the commit will be "silent" (won't necessarily cause new searchers to be opened)
    // CONSISTENT_COMMIT = 3; // FUTURE - ensure distributed searchers will see new data
  };
  CommitType commit = NO_COMMIT;
  // Deferred-commit window in milliseconds; meaningful only with commit != NO_COMMIT.
  // Positive on a plain commit (no maxSegments/aux/waitForMerges): the writer
  // downgrades the message to NO_COMMIT at the graph entry and guarantees an
  // auto-commit covering it within this window, coalescing across messages so a
  // stream of updates all carrying T produces ~one commit per T (see
  // IndexWriter::startUpdateBody).  0 = commit immediately.
  int64_t commit_within_ms = 0;
  int32_t maxSegments = 0;  // 0 means no client-requested merge target.

  // Internal synthetic commits use this to publish an already-produced segment
  // layout without flushing unrelated inverter state that arrived later.
  bool publishOnly = false;
  // Internal maintenance on commitFinishNode, without flushing or publication.
  bool applyOnly = false;
  std::optional<CommitId> resultingCommit;

  // Aux index rebuild request, applied during this commit (no effect if commit == NO_COMMIT).
  // See proto CommitParams.build_aux_indexes for semantics:
  //   empty       = no rebuild
  //   ["*"]       = rebuild all eligible
  //   ["vec.foo"] = rebuild this specific aux index
  std::vector<std::string> buildAuxIndexes;

  // Set to true to make this commit wait for any in-flight (or chained) merges
  // before it runs finishCommitBody.  Vector overlay builds deliberately do
  // not set this: per-segment validity is segment liveness, so a racing merge
  // can only waste a bounded build on a dying segment.
  bool waitForMerges = false;

  /// Filled in by the IndexWriter when the message is received.  Do not change.
  /// A message rejected at the graph entry (closed writer) keeps these defaults.
  uint64_t updateVersion = 0;         // Durable version used for document and delete ordering
  std::shared_ptr<Schema> schema;     // Pinned at writer admission, before assigning sequence numbers
  uint64_t updateOrdinal = 0;         // Session-local, 0-based update sequencer tag
  uint64_t commitNum = 0;             // The 0-based commit number of this update, used to ensure commits are finished in order
  std::unique_ptr<CommitInfo> commitInfo;  // Commit info for this update, if any.  This is set by the IndexWriter when the commit is processed.

  ErrorHolder result;

  // Partial updates (UpdateRequest.patches). They are resolved by this
  // message's own commit, against the documents that commit applied deletes
  // to, and the documents they produce are indexed by a follow-up message
  // under this message's update version (see IndexWriter::resolvePatches).
  std::span<const api::Patch> patches;
  struct PatchOutcome {
    api::PatchResult::Status status = api::PatchResult::Status::UNKNOWN;
    uint64_t version = 0;
    std::string error;
  };
  std::vector<PatchOutcome> patchOutcomes;  // one per patch, set by the resolving commit
  // The final state of each id the patches changed, indexed under updateVersion.
  struct PatchedDoc {
    std::string id;
    std::string body;    // JSON; empty when deleted
    bool deleted = false;
    std::vector<uint32_t> patchIndexes;  // the patches that produced it
  };
  std::vector<PatchedDoc> patchedDocs;
  int64_t dateMathNowMillis = 0;  // request clock for date math in patched documents
  bool patchPinned = false;       // IndexWriter::pinPatches holds delete retirement for it

  // Indexes patchedDocs into `inverter`, each under updateVersion, recording a
  // per-document failure in patchOutcomes. Messages that carry patches implement it.
  virtual void indexPatchedDocs(IndexWriter& iw, Inverter& inverter) {
    unused(iw);
    unused(inverter);
  }
};

class MergeMessage : public UpdateMessage {
public:
  int32_t mergeLevel = -1;  // Segment level to merge.  -1 means unspecified.
  UpdateMessage* forcedBy = nullptr;  // Client commit whose layout promise this merge completes.
};

}
