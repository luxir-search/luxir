#pragma once

#include <solux/query/ProtobufQueryParser.h>
#include <oneapi/tbb/flow_graph.h>
#include "solux/server/SoluxNode.h"
#include "protos/solux_types.pb.h"
#include "IndexReader.h"
#include "Collector.h"

namespace solux {

class SoluxNode;



/// The SearchEngine is a singleton owned by the SoluxNode object and is responsible for
/// executing search requests.  It is the main entry point for the search subsystem.
class SearchEngine {
  SoluxNode& node;
  // exccution graph
  oneapi::tbb::flow::graph graph;
  oneapi::tbb::task_group group;
public:
  SearchEngine(SoluxNode& node): node(node) {

  }

  // The top-level request for the SearchEngine.
  class Request {
    // Thoughts: if there are many different types of callbacks, then subclassing could be a better match.
    // if there is only one callback when done, then std::function could be a better match as it
    // allows for a lambda to be passed in, which can be much more concise.
  public:
    SearchEngine& engine;
    solux::proto::SearchRequest& proto;
    google::protobuf::Arena& reqArena;
    std::shared_ptr<IndexReader> reader;
    std::shared_ptr<Schema> schema;
    std::function<void(Request&)> callback;
    MemPool requestPool;

    Request(SearchEngine& engine, solux::proto::SearchRequest& proto, google::protobuf::Arena& reqArena): engine(engine), proto(proto), reqArena(reqArena) {
    }
  };

  // A response object that can be used to send back results to the client.
  // One or more responses can be sent back for a single request.
  class Response {
  public:
    SearchEngine::Request& req;
    solux::proto::SearchResponse* proto;
    google::protobuf::Arena* rspArena;

    // we could either create a data-structure that represents the dependencies, and
    // then query that data structure to determine what to do next, or we could
    // have a callback that is called when a stage of the request is done that
    // could launch new sub-requests.
    //
    // How is data passed from one stage to the next?  We could have a map here.
    // Or perhaps the callback could be called with the result (i.e. callbacks would be typed differently)
    // How to implement the join node (need to wait for 2 parts of a query?)
    // Data lifetime can vary a bit... think about sending query response back first, then facet response later.
    //
    // It feels like we are duplicating TBB graph stuff here.  Maybe we should do a high level wait-for-all per search
    // request?  BUT long streaming search would then lock out everyone else?

    Response(SearchEngine::Request& req, solux::proto::SearchResponse* proto=nullptr, google::protobuf::Arena* rspArena=nullptr): req(req), proto(proto), rspArena(rspArena) {
    }
  };

  class QueryReq {
  public:
    SearchEngine::Request& req;
    Query::Context& qcontext;
    Query* query;
    Query::Weight* weight;
    int64_t topCount;  // maximum number of docs to return.
    std::function<void(QueryReq&)> callback;

    // Concurrent merge strategy:
    // Rather than a mutex, we can just use a single atomic pointer to a TopDocsCollector.
    // On merge, if the pointer is null, just set.  If it's not null, grab it and merge, then try to set again.
    // This plays well with a single thread handling multiple segments (no merging necessary, should be close to
    // serial performance).
    // This is not algorithmically optimal merging (a PQ of collectors would be better), but this would result in less
    // memory usage. If we did want to delay merging, then we could still use the single-atomic-pointer
    // approach, and create a linked list of collectors to merge later.
    // This also allows us to maintain a counter to tell when we are done without having another atomic variable.

    struct CollectorHolder {
      TopDocsCollector collector;
      int32_t segmentsMerged;  // the number of segments that this collector represents
      bool heapAllocated;  // if true, call delete on the collector when it is no longer needed.
    };
    std::atomic<CollectorHolder*> collectorHolder;

    // TODO: consider getting rid of the "req" parameter to make this more generic (usable standalone, etc)
    QueryReq(SearchEngine::Request& req, Query::Context& qcontext, Query* query, int64_t topCount, std::function<void(QueryReq&)>callback={}): req(req), qcontext(qcontext), callback(std::move(callback)) {
      weight = query->createWeight(qcontext);
    }

    CollectorHolder* getCollector() {
      auto holder = collectorHolder.exchange(nullptr);
      if (holder == nullptr) {
        holder = new CollectorHolder(topCount);
      }
    }

    /// Merge one collector into the other and return the merged collector.  It could be either a or b.
    CollectorHolder* mergeCollector(CollectorHolder* a, CollectorHolder* b) {

      // If one is heap allocated and the other is not, then we can just merge into the non-heap allocated collector.
      if (a->heapAllocated ^ b->heapAllocated) {
        if (a->heapAllocated) {
          std::swap(a,b);
        }
      } else {
        // merge the smaller collector into the larger collector, or if both the same size, merge
        // the less competitive collector into the more competitive collector.
        if (a->collector.size() < b->collector.size()
            || a->collector.minCompetitiveVal < b->collector.minCompetitiveVal)
        {
          std::swap(a,b);
        }
      }

      a->collector.merge(b->collector);
      a->segmentsMerged += b->segmentsMerged;
      if (b->heapAllocated) {
        delete b;
      }

      return a;
    }

    /// returns true if all segments have been collected and merged
    bool releaseCollector(CollectorHolder* holder) {
      holder->segmentsMerged++;

      for(;;) {
        bool allSegsMerged = holder->segmentsMerged == req.reader->segments().size();
        holder = collectorHolder.exchange(holder);
        if (holder == nullptr) {
          return allSegsMerged;
        }
        // try to grab the other collector to merge
        auto other = collectorHolder.exchange(nullptr);
        if (other != nullptr) {
          holder = mergeCollector(holder, other);
        }
      }
      [[unreachable]];
    }

    void collect(int32_t segnum) {
      // get other resources we need first before obtaining the collector.



      auto holder = getCollector();

      if (releaseCollector(holder)) {
        // we are done, so we can call the callback
        callback(*this);
      }
    }




  };



  // Next question... where does the Query go for a query request?
  // TODO: named queries?

  // TODO: enable the ability to return partial results (i.e. query and facet results separately,
  // or multiple queries separately, or intermediate facet results!)

  void submit(SearchEngine::Request* req) {
    // we need to figure out what the dependencies are for the search request and then
    // fire off the top-level requests.

    // Example with 2 independent queries: for each query
    // we need to:
    // 1. create a Query object
    // 2. find top N ids
    // 3. load/fill-in stored fields
    // 4. reply with the results
    // NOTE: that step 3+4 may have many steps / responses if streaming back large result sets!
    // For replies we could:
    //  - send back a single response with all the results
    //  - send back separate results as each part is ready
    // We may not know ahead of time if the results will be large?  In which case, we'll need to
    //  decide to stream back more results even if client requested single-response (based on some limit?)
  }



  void submitBody(SearchEngine::Request& req) {
    // We probably want to parse all up-front in a single task so we understand all
    // of the dependencies.  We can always revisit if this becomes an issue.
    // In the future, things like acquiring the indexreader should also be done in a task since it could take a while.

    solux::proto::SearchRequest& proto = req.proto;

    for (auto& [opKey, searchOp] : proto.ops()) {
      switch (searchOp.kind_case()) {
        case solux::proto::SearchOp::kTopDocs: {
          auto& topDocsReq = searchOp.top_docs();
          /*
          message TopDocs {
                  Query query = 1;
                  int64 offset = 2;
                  optional sint64 limit = 3;
                  bool get_number = 4;          // return the number of matching documents
                  bool get_scores = 5;          // return the relevancy score for each document returned
                  repeated string fields = 6;   // fields to return for each document
                  repeated SortSpec sorts = 7;
          }
          */

          // Place the Query in the requestPool since it uses things like string_view that directly reference
          // the request.
          ProtobufQueryParser parser(req.requestPool, *req.schema);
          Query* query = parser.parse(topDocsReq.query());
          int64_t offset = topDocsReq.offset();
          unused(offset); // TODO
          int64_t specifiedLimit = topDocsReq.has_limit() ? topDocsReq.limit() : 10;
          // limit to actual number of docs in the index (or all if limit == -1)
          int64_t limit = specifiedLimit < 0 ? req.reader->numDocs() : std::min(specifiedLimit, req.reader->numDocs());

          auto* qcontext = google::protobuf::Arena::Create<Query::Context>(&req.reqArena, req.requestPool, *req.reader);
          auto* qr = google::protobuf::Arena::Create<QueryReq>(&req.reqArena, req, *qcontext, query, limit);

          // todo: set callback to do the next step

        } // end case
          break;
      } // end switch
    } // end for ever searchOp


  }


  void doFindTopN(QueryReq& qr) {




  }

  SearchEngine(const SearchEngine&) = delete;
  SearchEngine& operator=(const SearchEngine&) = delete;
};

} // namespace solux