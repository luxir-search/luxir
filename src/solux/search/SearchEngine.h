#pragma once

#include "SearchRequest.h"
#include "solux/server/SoluxNode.h"

namespace solux {

class SoluxNode;

/// The SearchEngine is a singleton owned by the SoluxNode object and is responsible for
/// executing search requests.  It is the main entry point for the search subsystem.
//
class SearchEngine {
  SoluxNode& node;

  void submitBody(SearchRequest& req);

  /// get needed resources such as the index reader and schema
  void getResources(SearchRequest& req);

public:
  SearchEngine(SoluxNode& node): node(node) {
  }

  // Most users should use this entry point to submit a search request.
  // maxParallel: 0 = auto (engine decides; today that means parallel),
  // 1 = single-threaded, >1 reserved (rejected as a request error).
  void submit(SearchRequest& req, int32_t maxParallel = 0);
  // bool converts to int silently and would flip meaning (false -> 0 = auto);
  // force old call sites to say what they mean.
  void submit(SearchRequest& req, bool) = delete;


  SearchEngine(const SearchEngine&) = delete;
  SearchEngine& operator=(const SearchEngine&) = delete;
};

} // namespace solux