#pragma once

// Turns a wire request into the op tree.
//
// DECLARATION ONLY: the implementation is in ProtobufSearchParser.cpp. The parser has to name
// every request type, so it pulls the whole ops/, query/ and value/ tree behind it - that used
// to be in this header, where a single #include cost ~12s and ~2GB to compile and forced the
// rule "include it in exactly one place per binary". Tests that only wanted to check the shape
// the parser produces had to be consolidated into one file to stay under that budget.
//
// That is no longer necessary: including this header is now cheap, so a test can construct a
// parser and call parse() wherever it makes sense. A test that then asserts on the op tree
// still includes the op headers it actually inspects (ops/TopDocsReq.h and friends) - it just
// no longer pays for the six it does not.
//
// The recursive descent over the op tree lives entirely inside the .cpp; callers cross this
// boundary once per request.

namespace solux {

class SearchOp;
class SearchRequest;

class ProtobufSearchParser {
  SearchRequest& req;

public:
  // The request's pool stores the parsed query tree, and its schema decides what types of
  // queries to produce. Both the pool and any parsed protobuf objects must outlive the tree.
  explicit ProtobufSearchParser(SearchRequest& req);

  SearchOp* parse();
};

} // solux
