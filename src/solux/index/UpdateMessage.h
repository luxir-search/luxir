#pragma once

#include <deque>
#include "Inverter.h"
#include "solux/server/SoluxError.h"


namespace solux {

class IndexWriter;
class UpdateMessage;

// CommitInfo is used to track information about a commit.  This is internally used by the IndexWriter.
// The IndexWriter has a global/next CommitInfo used to track deletions, and when a commit message is
// received, it is moved to that message and a new CommitInfo is created for the next commit.
// This really belongs internally to the IndexWriter, but since UpdateMessage isn't scoped within IndexWriter,
// this can't be either (can't forward declare IndexWriter::CommitInfo).
class CommitInfo {
public:
  UpdateMessage* updateMessage = nullptr;  // The update message that triggered this commit.
  uint64_t commitNum = 0;  // The commit number of this commit, used to ensure commits are finished in order

  // Number of segments left to flush, protected by same mutex that protects the inverter lists.
  // making this an atomic is not enough to avoid race conditions since we also depend on coordination with
  // inverter->updateMessage, among other things.
  uint32_t leftToFlush = 0;  // internal use only

  std::deque<Inverter::DeletesData> deletesList;  // deletes that need to be applied to all segments.
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

  // See docs in solux.proto:UpdateRequest
  // NOTE: These values should be kept in sync with the protobuf definition.
  enum CommitType {
    NO_COMMIT = 0,         // the default
    COMMIT = 1,            // ensure new data is searchable
    SILENT_COMMIT = 2,     // the commit will be "silent" (won't necessarily cause new searchers to be opened)
    // CONSISTENT_COMMIT = 3; // FUTURE - ensure distributed searchers will see new data
  };
  CommitType commit;
  int32_t commit_within;  // TODO: implement this


  // Filled in by the IndexWriter when the message is received.
  uint64_t seqNum;                    // The sequence number of this update, used to ensure updates are processed in order when needed
  uint64_t commitNum;                 // The commit number of this update, used to ensure commits are finished in order
  std::unique_ptr<CommitInfo> commitInfo;  // Commit info for this update, if any.  This is set by the IndexWriter when the commit is processed.

  ErrorHolder result;
};

class MergeMessage : public UpdateMessage {
public:
  int32_t mergeLevel = -1;  // Segment level to merge.  -1 means unspecified.
  int32_t maxSegments = 0;  // Merge down to this number of segments.
};

}