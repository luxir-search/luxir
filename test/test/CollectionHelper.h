#pragma once

#include "TestUtils.h"
#include "solux/index/IndexWriter.h"

namespace solux::test {


class CollectionHelper {
private:
  std::shared_ptr<Collection> collection_;
  std::counting_semaphore<1'000'000> indexSemaphore; // semaphore to limit concurrent indexing operations

  class SimpleUpdateMessage : public UpdateMessage {
  public:

    void indexMulti(IndexWriter& iw, std::span<const Doc> docs) {
      if (docs.empty()) {
        return; // nothing to index, avoid grabbing an inverter.
      }
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

        /*
        std::visit([&](auto&& arg) {
          // using T = std::decay_t<decltype(arg)>;
          // if constexpr (std::is_same_v<T, int64_t>)
          handler.index(inverter, arg);
        }, val);
        */

        std::visit(overloaded{
                [&](bool v){handler.index(inverter, v); },
                [&](int64_t v){handler.index(inverter, v); },
                [&](float v){handler.index(inverter, v); },
                [&](double v){handler.index(inverter, v); },
                [&](std::string v){handler.index(inverter, v); },
                [&](std::vector<bool> v){
                  unused(v);
                  // handler.index(inverter, v);
                  },
                [&](std::vector<int64_t> v){
                  handler.index(inverter, v);
                  },
                [&](std::vector<float> v){
                  unused(v);
                  // handler.index(inverter, v);
                  },
                [&](std::vector<double> v){
                  unused(v);
                  // handler.index(inverter, v);
                  },
                [&](std::vector<std::string> v){
                  // stack allocate... obviously not for anything large.
                  auto arr = (std::string_view*)alloca(v.size() * sizeof(std::string_view));
                  std::span<std::string_view> sv(arr, v.size());
                  std::ranges::copy(v, sv.begin());
                  handler.index(inverter, sv);
                }
        }, val);

      }
      inverter.finishDoc();
    }
  };

public:
  // indexConcurrency is the number of concurrent indexing operations allowed before blocking.
  CollectionHelper(std::string_view name = "main", size_t indexConcurrency = 100)
  : indexSemaphore(indexConcurrency)
  {
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
        // LOG_DEBUG("BlockingUpdateMessage done!");
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
  void index(Doc&& doc, std::function<void(ErrorHolder& result)>&& callback,
             UpdateMessage::CommitType commitType = UpdateMessage::NO_COMMIT) {
    index(std::vector<Doc>{doc}, std::move(callback), commitType);
  }


  // Async version of index.  Docs will be moved into the update message.
  void index(std::vector<Doc>&& docs, std::function<void(ErrorHolder& result)>&& callback,
             UpdateMessage::CommitType commitType = UpdateMessage::NO_COMMIT) {
    auto writer = collection().getShard()->getIndexWriter();

    class UpdateMessageWithCallback : public SimpleUpdateMessage {
    public:
      std::function<void(ErrorHolder& result)> callback;
      const std::vector<Doc> docs;

      UpdateMessageWithCallback(std::vector<Doc>&& docs) : docs(std::move(docs)) {
      }

      void handle(IndexWriter& iw) override {
        indexMulti(iw, docs);
      }

      void done(IndexWriter& iw) override {
        unused(iw);
        callback(result);
        delete this;
      }
    };

    UpdateMessageWithCallback* updateMessage = new UpdateMessageWithCallback(std::move(docs));
    updateMessage->commit = commitType;
    updateMessage->callback = std::move(callback);
    auto success = writer->submitUpdate(updateMessage);
    assert(success);
  }

  /// Removes all data in the collection.
  void clear() {
    auto writer = collection().getShard()->getIndexWriter();
    writer->testDeleteAllData();
  }


  // Directly get a handler to the IndexWriter for more low-level control of indexing operations.
  std::shared_ptr<IndexWriter> getIndexWriter() {
    return collection().getShard()->getIndexWriter();
  }

  /// Given a number of documents, a merge factor, and an index shape, calculate the number of
  /// documents that should be in each segment.  The "shape" is a string that lists the
  /// number of segments at each level, starting with the largest level.
  /// For example, a shape of "935" means 9 large segments, 3 segments roughly mergeSegment smaller, and 5 segments
  /// roughly mergeSegment smaller than that.
  /// The docsPerSeg vector will be filled in with the number of documents in each segment, largest to smallest.
  static void calcSegSizes(int64_t nDocs, int mergeFactor, std::string_view shape, std::vector<int32_t>& docsPerSeg) {
    docsPerSeg.clear();
    std::vector<int32_t> segsPerTier;
    for (char c : shape) {
      if (c > '9') {
        segsPerTier.push_back(c-'a' + 10);  // base 36 - convert 'a' to 10, 'b' to 11, etc.
      } else {
        segsPerTier.push_back(c - '0');  // convert char to int
      }
    }

    double totalWeight = 0.0;
    double currentLevelSize = 1;
    // slightly larger than mergeFactor to avoid rounding errors putting different tier segments at the same level
    // according to the mergePolicy in IndexWriter.
    double effectiveMergeFactor = mergeFactor * 1.05;

    // First, calculate the total "weight" of all segments combined.
    // We start from the smallest segments (end of the shape string) and move to the largest.
    for (int i = shape.length() - 1; i >= 0; --i) {
      totalWeight += segsPerTier[i] * currentLevelSize;
      currentLevelSize *= effectiveMergeFactor;
    }

    // If there's no weight, we can't distribute documents.
    if (totalWeight == 0) {
      return;
    }

    // now calculate what everything should be scaled by
    double scale = nDocs / totalWeight;

    // Now, calculate the size of each segment, from largest to smallest level.
    int power = segsPerTier.size() - 1;

    for (auto nSegs : segsPerTier) {
      int32_t docs = scale * std::pow(effectiveMergeFactor, power);
      if (docs < 1) break; // no more segments to add, we are done.
      for (int i = 0; i < nSegs; ++i) {
        docsPerSeg.push_back(docs);
      }
      power--;
    }

    // Find how much we are off from nDocs.
    int64_t diff = nDocs - std::accumulate(docsPerSeg.begin(), docsPerSeg.end(), 0LL);

    // if we need more docs, add to largest segment to avoid triggering a merge.
    if (diff > 0) {
      docsPerSeg[0] += diff; // add to the first segment
    } else if (diff < 0) {
      // too many docs... reduce size of smallest segment.
      docsPerSeg.back() += diff;
      assert(docsPerSeg.back() >= 0); // should never go negative or 0
    }
  }

};


} // solux::test