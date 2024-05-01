#pragma once

#include "SoluxTest.h"
#include "solux/index/IndexWriter.h"

namespace solux::test {

// Idea: think of making a reference-version (i.e. std::string_view, std::span) of this class for use in main code.

using FieldVal = std::variant<bool, int64_t, float, double, std::string>;

struct NameVal {
  std::string name;
  FieldVal val;
};

using Doc = std::vector<NameVal>;

// allow construction of a Doc with just alternating names and values. Example:
// auto doc1 = flatdoc("name1", 1, "name2", 2.0, "name3", "hi");
template<typename T1, typename T2, typename... Args>
Doc flatdoc(T1 arg1, T2 arg2, Args... args) {
  std::vector<NameVal> vec;
  vec.push_back(NameVal{arg1, arg2});
  if constexpr (sizeof...(args) > 0) {
    auto otherPairs = flatdoc(args...);
    vec.insert(vec.end(), otherPairs.begin(), otherPairs.end());
  }
  return vec;
};

/*
 * class UpdateMessage {
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

  // Number of segments left to flush, protected by same mutex that protects the inverter lists.
  // making this an atomic is not enough to avoid race conditions since we also depend on coordination with
  // inverter->updateMessage, among other things.
  uint32_t leftToFlush = 0;  // internal use only
};
 */


class CollectionHelper {
private:
  std::shared_ptr<Collection> collection_;

  class SimpleUpdateMessage : public UpdateMessage {
  public:

    void indexMulti(IndexWriter& iw, std::span<const Doc> docs) {
      auto& inverter = iw.obtainInverter();
      for (auto& doc: docs) {
        indexSingle(inverter, doc);
      }
      iw.releaseInverter(inverter);
    }

    void indexSingle(Inverter& inverter, const Doc& doc) {
      inverter.startDoc();
      for (auto& nv: doc) {
        auto& handler = inverter.getIndexHandler(nv.name);
        auto& val = nv.val;
        switch (val.index()) {
          case 0:
            handler.index(inverter, std::get<bool>(val));
            break;
          case 1:
            handler.index(inverter, std::get<int64_t>(val));
            break;
          case 2:
            handler.index(inverter, std::get<float>(val));
            break;
          case 3:
            handler.index(inverter, std::get<double>(val));
            break;
          case 4: {
            std::string copy = std::get<std::string>(val);  // TODO: revisit changing the input string in the indexer!
            handler.index(inverter, copy.data(), copy.size());
            break;
          }
          default:
            throw std::runtime_error("Unknown type in Doc");
        }
      }
      inverter.finishDoc();
    }
  };

public:
  CollectionHelper(std::string_view name = "main") {
    collection_ = SoluxTest::soluxNode->getCollection(name);
  }

  Collection& collection() {
    return *collection_;
  }

  void commit() {
    collection().getShard()->getIndexWriter()->commit();
  }

  void index(const Doc& doc, UpdateMessage::CommitType commitType = UpdateMessage::NO_COMMIT) {
    index({&doc,1}, commitType);
  }

  void index(std::span<const Doc> docs, UpdateMessage::CommitType commitType = UpdateMessage::NO_COMMIT) {
    auto writer = collection().getShard()->getIndexWriter();

    class BlockingUpdateMessage : public SimpleUpdateMessage {
    public:
      std::span<const Doc> docs;
      Blocker blocker;

      explicit BlockingUpdateMessage(std::span<const Doc> docs) : docs(docs) {
      }

      void handle(IndexWriter& iw) override {
        indexMulti(iw, docs);
      }

      void done(IndexWriter& iw) override {
        unused(iw);
        LOG_DEBUG("BlockingUpdateMessage done!");
        blocker.notify();
      }
    };


    // all stack allocated since we will be waiting for completion.
    BlockingUpdateMessage updateMessage(docs);
    updateMessage.commit = commitType;

    bool success = writer->submitUpdate(&updateMessage);
    assert(success);

    updateMessage.blocker.wait();
  }

  // Async version of index.  Docs will be moved into the update message.
  void index(const Doc&& doc, std::function<void()>&& callback,
             UpdateMessage::CommitType commitType = UpdateMessage::NO_COMMIT) {
    index(std::vector<Doc>{doc}, std::move(callback), commitType);
  }


  // Async version of index.  Docs will be moved into the update message.
  void index(const std::vector<Doc>&& docs, std::function<void()>&& callback,
             UpdateMessage::CommitType commitType = UpdateMessage::NO_COMMIT) {
    auto writer = collection().getShard()->getIndexWriter();

    class UpdateMessageWithCallback : public SimpleUpdateMessage {
    public:
      std::function<void()> callback;
      const std::vector<Doc> docs;

      UpdateMessageWithCallback(const std::vector<Doc>&& docs) : docs(std::move(docs)) {
      }

      void handle(IndexWriter& iw) override {
        indexMulti(iw, docs);
      }

      void done(IndexWriter& iw) override {
        unused(iw);
        callback();
        delete this;
      }
    };

    UpdateMessageWithCallback* updateMessage = new UpdateMessageWithCallback(std::move(docs));
    updateMessage->commit = commitType;
    updateMessage->callback = std::move(callback);
    auto success = writer->submitUpdate(updateMessage);
    assert(success);
  }

};


} // solux::test