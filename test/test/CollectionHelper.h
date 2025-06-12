#pragma once

#include "TestUtils.h"
#include "solux/index/IndexWriter.h"
#include "solux/server/ProtoUpdateMessage.h"
#include <google/protobuf/arena.h>

namespace solux::test {


class CollectionHelper {
private:
  std::shared_ptr<Collection> collection_;
  std::counting_semaphore<1'000'000> indexSemaphore; // semaphore to limit concurrent indexing operations


  // Helper function to convert Doc to protobuf Map
  static void convertDocToProto(const Doc& doc, proto::Map& map) {

    for (const auto& nv : doc) {
      auto& val = (*map.mutable_fields())[nv.name];
      
      std::visit(overloaded{
        [&](bool v) { val.set_b(v); },
        [&](int64_t v) { val.set_i(v); },
        [&](float v) { val.set_f(v); },
        [&](double v) { val.set_d(v); },
        [&](const std::string& v) { val.set_s(v); },
        [&](const std::vector<bool>& v) {
          unused(v);
          // TODO: handle bool arrays if needed
        },
        [&](const std::vector<int64_t>& v) {
          auto* arr = val.mutable_arr_i();
          for (auto i : v) {
            arr->add_v(i);
          }
        },
        [&](const std::vector<float>& v) {
          auto* arr = val.mutable_arr_f();
          for (auto f : v) {
            arr->add_v(f);
          }
        },
        [&](const std::vector<double>& v) {
          auto* arr = val.mutable_arr_d();
          for (auto d : v) {
            arr->add_v(d);
          }
        },
        [&](const std::vector<std::string>& v) {
          auto* arr = val.mutable_arr_s();
          for (const auto& s : v) {
            arr->add_v(s);
          }
        }
      }, nv.val);
    }
  }

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

    class BlockingProtoUpdateMessage : public ProtoUpdateMessage {
    public:
      Blocker blocker;
      
      explicit BlockingProtoUpdateMessage(proto::UpdateRequest* req) 
        : ProtoUpdateMessage(req) {
      }

      void done(IndexWriter& iw) override {
        unused(iw);
        blocker.notify();
      }
    };

    google::protobuf::Arena arena;
    auto* request = google::protobuf::Arena::Create<proto::UpdateRequest>(&arena);
    
    // Convert docs to protobuf format
    for (const auto& doc : docs) {
      convertDocToProto(doc, *request->add_docs());
    }
    
    // Set commit type
    request->set_commit(static_cast<proto::UpdateRequest::CommitType>(commitType));
    
    // Create and submit the update message
    BlockingProtoUpdateMessage updateMessage(request);
    
    bool success = writer->submitUpdate(&updateMessage);
    assert(success);
    unused(success);

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

    class ProtoUpdateMessageWithCallback : public ProtoUpdateMessage {
    public:
      std::function<void(ErrorHolder& result)> callback;
      std::unique_ptr<google::protobuf::Arena> arena;
      
      ProtoUpdateMessageWithCallback(proto::UpdateRequest* req, std::unique_ptr<google::protobuf::Arena> arena) 
        : ProtoUpdateMessage(req), arena(std::move(arena)) {
      }

      void done(IndexWriter& iw) override {
        unused(iw);
        callback(result);
        delete this;
      }
    };

    // Create protobuf request on heap-allocated arena (will be deleted in done())
    auto arena = std::make_unique<google::protobuf::Arena>();
    auto* request = google::protobuf::Arena::Create<proto::UpdateRequest>(arena.get());
    
    // Convert docs to protobuf format
    for (const auto& doc : docs) {
      convertDocToProto(doc, *request->add_docs());
    }
    
    // Set commit type
    request->set_commit(static_cast<proto::UpdateRequest::CommitType>(commitType));
    
    auto* updateMessage = new ProtoUpdateMessageWithCallback(request, std::move(arena));
    updateMessage->callback = std::move(callback);
    
    auto success = writer->submitUpdate(updateMessage);
    assert(success);
    unused(success);
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

  //
  // returns true if the current index matches the shape of the given docsPerSeg.
  // This is for the purpose of reusing the index from the previous benchmark / test.
  //
  bool indexMatchesShape(std::span<const int32_t> docsPerSeg) {
    // See if we can reuse the index from the previous benchmark.
    auto iw = getIndexWriter();

    // get the IndexReader
    auto reader = iw->getIndexReader();
    auto readerSegs = reader->segments().size();
    bool reuseIndex = readerSegs == docsPerSeg.size();
    // check each segment size
    if (reuseIndex) {
      for (size_t i=0; i<docsPerSeg.size(); i++) {
        auto& seg = reader->segments()[i];
        if (seg.postingsReader().numDocs() != docsPerSeg[i]) {
          reuseIndex = false;
          break;
        }
      }
    }
    return reuseIndex;
  }

};


} // solux::test