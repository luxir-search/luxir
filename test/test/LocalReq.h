#pragma once

#include "TestUtils.h"

namespace solux::test {


class LocalReq : public SearchRequest {
public:
  std::vector<SearchResponse*> responses;

  /// Heap allocate an Arena (if null) and use it to create a LocalReq object and proto::SearchRequest
  static LocalReq* create(SearchEngine& engine, google::protobuf::Arena* arena = nullptr) {
    arena = arena ? arena : createArena();
    auto* SearchRequestProto = google::protobuf::Arena::Create<solux::proto::SearchRequest>(arena);
    auto* localReq = google::protobuf::Arena::Create<LocalReq>(arena, engine, *SearchRequestProto);
    return localReq;
  }

  LocalReq(SearchEngine& engine, solux::proto::SearchRequest& proto) : SearchRequest(engine, proto) {
  }

  virtual ~LocalReq() {
    for (auto* response : responses) {
      if (&response->arena != &arena) {
        LOG_TRACE("releasing response arena!");
        releaseArena(&response->arena);
      }
    }
  }

  int reply(SearchResponse& response) override {
    responses.push_back(&response);
    /*
    std::string reqStr;
    google::protobuf::TextFormat::PrintToString(response.proto, &reqStr);
    LOG_DEBUG("\tresponse:{}", reqStr);
     */
    return 0;
  }

  // will never be called
  void replyCallback(SearchResponse& response) override {
    unused(response);
  }

  // should be called by user
  void done() override {
    releaseArena(&arena);
  }

  std::string toString() {
    std::string ret;
    ret += "Request:" + proto.DebugString() + "\n";
    for (auto* response : responses) {
      ret += "\tResponse:" + response->proto.DebugString() + "\n";
    }
    return ret;
  }

  // Convenience methods to make common operations easier

  // Set collection name easily
  LocalReq& collection(const std::string& name) {
    proto.mutable_collection()->add_name(name);
    return *this;
  }

  // Get or create a top_docs operation with the given name
  proto::TopDocs& topDocs(const std::string& opName = "q") {
    return *proto.mutable_ops()->operator[](opName).mutable_top_docs();
  }

  // Add a match query to a top_docs operation
  LocalReq& matchQuery(const std::string& field, const std::string& value, const std::string& opName = "q") {
    auto& query = *topDocs(opName).mutable_query()->mutable_match();
    query.set_field(field);
    query.mutable_val()->set_s(value);
    return *this;
  }

  // Add an "all documents" query
  LocalReq& allQuery(const std::string& opName = "q") {
    topDocs(opName).mutable_query()->set_all(true);
    return *this;
  }

  // Add fields to return in search results
  LocalReq& fields(std::initializer_list<std::string> fieldNames, const std::string& opName = "q") {
    auto& td = topDocs(opName);
    for (const auto& field : fieldNames) {
      *td.mutable_fields()->Add() = field;
    }
    return *this;
  }

  // Set limit for number of results
  LocalReq& limit(int64_t maxResults, const std::string& opName = "q") {
    topDocs(opName).set_limit(maxResults);
    return *this;
  }

  // Enable match count and scores
  LocalReq& withStats(const std::string& opName = "q") {
    auto& td = topDocs(opName);
    td.set_get_number(true);
    td.set_get_scores(true);
    return *this;
  }

  // Execute the search
  LocalReq& execute(bool parallel = true) {
    engine.submit(*this, parallel);
    return *this;
  }

  // Get the search results as Doc objects for easy comparison
  std::vector<Doc> getDocs(const std::string& opName = "q") {
    if (responses.empty()) {
      return {};
    }
    
    auto it = responses[0]->proto.ops().find(opName);
    if (it == responses[0]->proto.ops().end() || !it->second.has_docs()) {
      return {};
    }
    
    return convertResultsToDocs(it->second.docs());
  }

  // Get number of matches
  int64_t getMatchCount(const std::string& opName = "q") {
    if (responses.empty()) return 0;
    auto it = responses[0]->proto.ops().find(opName);
    if (it == responses[0]->proto.ops().end() || !it->second.has_docs()) {
      return 0;
    }
    return it->second.docs().matches();
  }

private:
  // Convert protobuf search results back to Doc format
  static std::vector<Doc> convertResultsToDocs(const proto::DocList& docs) {
    std::vector<Doc> results;
    
    if (docs.columns().empty()) {
      return results;
    }
    
    // Determine number of documents from any column
    size_t numDocs = 0;
    for (const auto& [fieldName, column] : docs.columns()) {
      if (fieldName == "_score_") continue; // Skip score field for document count
      
      if (column.has_col_s()) {
        numDocs = column.col_s().v_size();
      } else if (column.has_col_i()) {
        numDocs = column.col_i().v_size();
      } else if (column.has_col_f()) {
        numDocs = column.col_f().v_size();
      } else if (column.has_col_d()) {
        numDocs = column.col_d().v_size();
      } else if (column.has_multi_s()) {
        numDocs = column.multi_s().v_size();
      } else if (column.has_multi_i()) {
        numDocs = column.multi_i().v_size();
      } else if (column.has_multi_f()) {
        numDocs = column.multi_f().v_size();
      } else if (column.has_multi_d()) {
        numDocs = column.multi_d().v_size();
      }
      break;
    }
    
    results.resize(numDocs);
    
    // Convert each field's column data back to Doc format
    for (const auto& [fieldName, column] : docs.columns()) {
      if (fieldName == "_score_") continue; // Skip score field
      
      for (size_t docIdx = 0; docIdx < numDocs; docIdx++) {
        if (column.has_col_s()) {
          const auto& colData = column.col_s();
          if (docIdx < (size_t)(colData.v_size()) && colData.v(docIdx) != colData.missing_val()) {
            results[docIdx].push_back({fieldName, std::string(colData.v(docIdx))});
          }
        } else if (column.has_col_i()) {
          const auto& colData = column.col_i();
          if (docIdx < (size_t)(colData.v_size()) && colData.v(docIdx) != colData.missing_val()) {
            results[docIdx].push_back({fieldName, (int64_t)(colData.v(docIdx))});
          }
        } else if (column.has_col_f()) {
          const auto& colData = column.col_f();
          if (docIdx < (size_t)(colData.v_size()) && colData.v(docIdx) != colData.missing_val()) {
            results[docIdx].push_back({fieldName, colData.v(docIdx)});
          }
        } else if (column.has_col_d()) {
          const auto& colData = column.col_d();
          if (docIdx < (size_t)(colData.v_size()) && colData.v(docIdx) != colData.missing_val()) {
            results[docIdx].push_back({fieldName, colData.v(docIdx)});
          }
        } else if (column.has_multi_s()) {
          const auto& colData = column.multi_s();
          if (docIdx < (size_t)(colData.v_size()) && colData.v(docIdx).v_size() > 0) {
            std::vector<std::string> values;
            for (const auto& val : colData.v(docIdx).v()) {
              values.push_back(std::string(val));
            }
            results[docIdx].push_back({fieldName, std::move(values)});
          }
        } else if (column.has_multi_i()) {
          const auto& colData = column.multi_i();
          if (docIdx < (size_t)(colData.v_size()) && colData.v(docIdx).v_size() > 0) {
            std::vector<int64_t> values;
            for (const auto& val : colData.v(docIdx).v()) {
              values.push_back((int64_t)(val));
            }
            results[docIdx].push_back({fieldName, std::move(values)});
          }
        } else if (column.has_multi_f()) {
          const auto& colData = column.multi_f();
          if (docIdx < (size_t)(colData.v_size()) && colData.v(docIdx).v_size() > 0) {
            std::vector<float> values;
            for (const auto& val : colData.v(docIdx).v()) {
              values.push_back(val);
            }
            results[docIdx].push_back({fieldName, std::move(values)});
          }
        } else if (column.has_multi_d()) {
          const auto& colData = column.multi_d();
          if (docIdx < (size_t)(colData.v_size()) && colData.v(docIdx).v_size() > 0) {
            std::vector<double> values;
            for (const auto& val : colData.v(docIdx).v()) {
              values.push_back(val);
            }
            results[docIdx].push_back({fieldName, std::move(values)});
          }
        }
      }
    }
    
    return results;
  }
};


} // solux::test