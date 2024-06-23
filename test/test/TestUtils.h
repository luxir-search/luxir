#pragma once

#include <array>
#include "SoluxTest.h"
#include "solux/index/IndexWriter.h"

namespace solux::test {

// Idea: think of making a reference-version (i.e. std::string_view, std::span) of this class for use in main code.

using FieldVal = std::variant<bool, int64_t, float, double, std::string,
                              std::vector<bool>, std::vector<int64_t>, std::vector<float>, std::vector<double>, std::vector<std::string>
>;

struct NameVal {
  std::string name;
  FieldVal val;
};

using Doc = std::vector<NameVal>;

template <typename... Args>
constexpr auto arr(Args&&... args) {
  return std::to_array({std::forward<Args>(args)...});
}

template<typename... Args>
auto vec(Args&&... args) {
  return std::vector{std::forward<Args>(args)...};
}

// make a vector of string (as opposed to string_view or const char*)
template<typename... Args>
auto vecs(Args&&... args) {
  return std::vector<std::string>{std::forward<Args>(args)...};
}

// make a vector of string_view (as opposed to string or const char*)
template<typename... Args>
auto vecsv(Args&&... args) {
    return std::vector<std::string_view>{std::forward<Args>(args)...};
}

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


template<class... Ts>
struct overloaded : Ts... { using Ts::operator()...; };
template<class... Ts>
overloaded(Ts...) -> overloaded<Ts...>;

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
};


} // solux::test