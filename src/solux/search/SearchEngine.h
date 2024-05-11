#pragma once

#include <solux/query/ProtobufQueryParser.h>
#include <oneapi/tbb/flow_graph.h>
#include <solux/server/SoluxNode.h>
#include <solux/util/proto.h>
#include "protos/solux_types.pb.h"
#include "IndexReader.h"
#include "Collector.h"

namespace solux {

class SoluxNode;
class Collection;
class Library;
class IndexReader;
class Schema;


/// The SearchEngine is a singleton owned by the SoluxNode object and is responsible for
/// executing search requests.  It is the main entry point for the search subsystem.
//
// NOTES:
//  Lifetime management is the difficult part to coordinate between the Request and the GRPC Call (SearcherSearchStreamingCall) object.
//  1) The engine needs to say what response is the final response for this request.  That can result in the Call object being deleted
//     after the response callback is called.
//  2) After the callback for the final response is called, the engine calls Request.done() which typically deletes the Request object.
//  3) The request object being deleted is asynchronous and can happen before or after SearchEngine::submit returns.
//
//  Error handling:
//    If an exception is thrown, we need to send back an error response if we haven't already sent the final response.
//    This might happen concurrently while other tasks are executing... tough to coordinate! Failure to send a
//    final response would keep the StreamingCall alive indefinitely.
//
//    We could wait for all tasks to finish before checking if we've sent a final response, and if not, do so.
//    Even this is hard to implement, because new work may be launched asynchronously from the response callback (a natural
//    way to implement streaming while limiting buffering).  We actually can't even look at "req" any longer to see if
//    the final response was sent since it could have been deleted already!
//
//    Proposed Error handling solution:
//      - Have a single place to send the final response (*after* the top level task_group.wait() returns).
//        This ensures that everything else except the final response is done and we know exactly what
//        state we are in.
//      - To handle the issue of thinking we are done, but more work will be kicked off via a response callback,
//        we would need to keep a task alive (in a wait / send loop that work steals), or call task_group.reserve()
//        to keep the task_group alive until we are really done.
//
// In the future, we could keep track of active requests in the SearchEngine as a way to cancel them.
//
class SearchEngine {
  SoluxNode& node;

  // oneapi::tbb::flow::graph graph;
public:
  SearchEngine(SoluxNode& node): node(node) {
  }

  class Response;

  // The top-level request for the SearchEngine.
  class Request {
    friend class SearchEngine;
  public:
    SearchEngine& engine;
    // we should try to minimize the amount that these protobuf classes are used in case we need to move to a
    // more efficient implementation later.  Or even different formats like arrow.
    solux::proto::SearchRequest& proto;
    google::protobuf::Arena& arena;
    std::shared_ptr<IndexReader> reader;
    std::shared_ptr<Schema> schema;
    MemPool requestPool;
    oneapi::tbb::task_group* tg = nullptr;  // optional top-level task group for this request.
    Response* lastResponse = nullptr;

    Request(SearchEngine& engine, solux::proto::SearchRequest& proto): engine(engine), proto(proto), arena(*proto.GetArena()) {
    }
    virtual ~Request() = default;

    /// Call this to send a response back to the client (or cause it to be buffered).  This can be called multiple times for a single request.
    /// replyComplete will be called when the response has actually been written and is no longer needed.
    /// Returns the number of buffered responses (0 if the response was written immediately).
    virtual int reply(Response& response) = 0;

    /// This is called when the reply has completed (e.g. it has been written to a socket and not just buffered)
    /// This may be called from a gRPC server thread (and with a mutex locked), so it should be fast.
    virtual void replyCallback(Response& response) {
      bool last = response.last;
      // if this response used a different Arena, release it.
      if (&response.arena != &arena) {
        releaseArena(&response.arena);
      }
      if (last) {
        done();
      }
    };

    /// This will be called last after all responses have been sent back to the client.
    /// This is the place to do final cleanup (such as deleting or resetting the Arena)
    /// It may delete *this*, so nothing else should be accessed after this is called.
    virtual void done() {
      releaseArena(&arena);
    };
  };

  // A response object that can be used to send back results to the client.
  // One or more responses can be sent back for a single request.
  class Response {
  public:
    SearchEngine::Request& req;
    google::protobuf::Arena& arena;
    solux::proto::SearchResponse& proto;
    bool last = true;  // if true, this is the last response for the request.
    // TODO: we could add a callback here to facilitate chaining of responses (i.e. for streaming results, etc)

    Response(SearchEngine::Request& req, google::protobuf::Arena& arena, bool last)
    : req(req), arena(arena), proto(*google::protobuf::Arena::Create<solux::proto::SearchResponse>(&arena)), last(last)
    {
    }

    // Arena allocate a Response object.
    static Response* create(SearchEngine::Request& req, bool last=true) {
      // If this is the last message, just use the same arena as the response since they will have
      // the same lifetimes.
      auto* arena = last ? &req.arena : createArena();
      return google::protobuf::Arena::Create<Response>(arena, req, *arena, last);
    }
  };

  class QueryReq {
  public:
    SearchEngine::Request& req;
    Query::Context& qcontext;
    Query* query;
    Query::Weight* weight;
    int64_t topCount;  // maximum number of docs to return.

    std::string_view name;  // what search operation was this for?
    const solux::proto::TopDocs* topDocsProto;  // the relevant part of the protobuf request

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

    //  req, *qcontext, query, limit
    QueryReq(SearchEngine::Request& req, Query::Context& qcontext, Query* query, int64_t topCount, std::function<void(QueryReq&)>callback={})
    : req(req), qcontext(qcontext), query(query), topCount(topCount), callback(std::move(callback)) {
      weight = query->createWeight(qcontext);
    }

    CollectorHolder* getCollector() {
      auto* holder = collectorHolder.exchange(nullptr);
      if (holder == nullptr) {
        holder = new CollectorHolder({TopDocsCollector(topCount), 0, true});
      }
      return holder;
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
        bool allSegsMerged = (holder->segmentsMerged == req.reader->segments().size());
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
      CollectorHolder* holder = nullptr;
      {
        auto poolGuard = MemPool::threadLocalPoolGuard();
        auto scorer = weight->createScorer(poolGuard.pool(), qcontext.topReader.segments()[segnum]);

        // wait until last moment to obtain collector.
        holder = getCollector();
        auto& collector = holder->collector;
        for (;;) {
          auto doc = scorer->next();
          if (doc == PostingsReader::END) {
            break;
          }
          auto score = scorer->score();
          collector.collect(segnum, doc, score);
        }
      }

      if (releaseCollector(holder)) {
        // we are done, so we can call the callback
        // we could use a nested task_group to wait until we are done here as well.
        callback(*this);
      }
    }

    void start(oneapi::tbb::task_group* tg) {
      for (int32_t i=0; i<req.reader->segments().size(); i++) {
        task_group_run(tg, [this, i]() {
          this->collect(i);
        });
      }

      // handle special case of no segments.
      if (req.reader->segments().size() == 0) {
        // we are done, so we can call the callback
        callback(*this);
      }
    }
  };





  void submit(SearchEngine::Request& req) {
    try {
      submitBody(req);



    } catch (std::exception& e) {
      LOG_ERROR("Unexpected exception: {}", e.what());

    }
  }


  void submitBody(SearchEngine::Request& req) {
    getResources(req);

    req.lastResponse = Response::create(req, true);
    req.lastResponse->proto.set_request_id(req.proto.request_id());


    // We probably want to parse all up-front in a single task to understand dependencies.

    solux::proto::SearchRequest& proto = req.proto;

    std::vector<QueryReq*> queryReqs;

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

          auto* qcontext = google::protobuf::Arena::Create<Query::Context>(&req.arena, req.requestPool, *req.reader);
          auto* qr = google::protobuf::Arena::Create<QueryReq>(&req.arena, req, *qcontext, query, limit);
          qr->callback = [this](QueryReq& qr) {
            fillQueryTopNResponse(qr);
            // If faceting is done per-segment, that could be kicked off after each segment is done.
            // If so, should probably have a separate task_group to launch the faceting tasks? Depends on how
            // we use the task_group for the query (call wait vs configure a callback)
          };
          qr->name = opKey;  // stringview to the key in the map.  This *should* be stable give that we don't modify the request.
          qr->topDocsProto = &topDocsReq;

          queryReqs.push_back(qr);
        } // end case
          break;
      } // end switch
    } // end for ever searchOp


    for (auto& [opKey, searchOp] : proto.ops()) {
      switch (searchOp.kind_case()) {
        case solux::proto::SearchOp::kFieldFacet: {
          auto& facetReq = searchOp.field_facet();
        } // end case
          break;
        default:
          // already handled, or ignoring for now
          break;
      } // end switch
    }

    // launch all top-level queries
    for (auto qr : queryReqs) {
      qr->start(req.tg);
    }

    // do we need to special case when there we no queries or facets?
    if (req.tg != nullptr) {
      req.tg->wait();
    }

    // send back the final response
    req.reply(*req.lastResponse);
  }


  /// get needed resources such as the index reader and schema
  void getResources(SearchEngine::Request& req);


  // This is currently called after all segments have been collected for a TopN query to
  // fill out a DocList proto message.
  void fillQueryTopNResponse(QueryReq& qr) {
    // for now, just use the response object from the request.
    auto& response = *qr.req.lastResponse;

    auto& searchResultProto = response.proto.mutable_ops()->operator[](qr.name);
    auto& docListProto = *searchResultProto.mutable_docs();


//    message DocList {
//       sint64 matches = 1;
//       float max_score = 2;
//       Columns columns = 3;
//       int64 offset = 4;
//       bool more = 5; // expect more results to be streamed back?
//    }

    // grab the TopDocs from the collector and fill in the fields.
    auto* collectorHolder = qr.collectorHolder.load(std::memory_order_relaxed);

    if (collectorHolder == nullptr) {
      // no results
      docListProto.set_matches(0);
      return;
    }

    auto& collector = collectorHolder->collector;
    docListProto.set_offset(0);

    if (qr.topDocsProto->get_number()) {
      docListProto.set_matches(collector.totalHits());
    }

    // Should we somehow do auto-sizing of return messages?
    // We could load largest field first and cut it off after it gets big enough.  That can't really be
    // parallelized easily though.
    // We could also do it based on index data of the average column size.

    int columnSize = collector.size();  // return all docs in the collector for now.

    if (columnSize > 0) {
      // std::optional keeps constructor from being called if not needed.  We could also pool allocate it.
      std::optional<oneapi::tbb::task_group> loadColumnsTaskGroup;
      oneapi::tbb::task_group* tg = qr.req.tg ? &loadColumnsTaskGroup.emplace() : nullptr;

      bool returnScores = qr.topDocsProto->get_scores();
      collector.sort();

      // sort the documents so we can access them in order of both segment and docid
      std::vector<uint8_t> sortedIdx(columnSize); // works up to 256 docs.
      for (int i=0; i<columnSize; i++) {
        sortedIdx[i] = i;
      }
      // for small int lists, std::sort is faster than radix sort.
      auto& topDocs = collector.topDocs;
      std::sort(sortedIdx.begin(), sortedIdx.end(), [&topDocs](auto a, auto b) {
        return topDocs[a].doc < topDocs[b].doc;
      });
      // calculate the segment runs just once so they can be used to load multiple fields.
      std::vector<uint8_t> sortedIdxRunLen(columnSize);  // how many docs in a row are from the same segment
      for (int i=0; i<columnSize;) {
        auto seg = topDocs[sortedIdx[i]].doc.segment();
        int runLen = 1;
        while (i+runLen < columnSize && topDocs[sortedIdx[i+runLen]].doc.segment() == seg) {
          runLen++;
        }
        sortedIdxRunLen[i] = runLen;
        i += runLen;
      }

      auto& columnsProto = *docListProto.mutable_columns();

      // Lets look at the stored fields
      for (std::string_view field : qr.topDocsProto->fields()) {
        // should we allow _scores_ as a field name?
        if (field == "_score_") {
          // TODO: if we are going to allow this, we would need to check earlier int he code to make sure we are recording scores.
          returnScores = true;
          continue;
        }

        // look up the field in the schema
        auto& fieldType = *qr.req.schema->getFieldTypeEx(field);
        switch (fieldType.type()) {
          case FieldType::Type::INT: {
            auto& intColProto = columnsProto[field];
            auto& intCol = *intColProto.mutable_col_i();
            auto& intsProto = *intCol.mutable_v();
            // intCol.set_missing_val(0); // TODO.... get from schema? Set even if all values present?
            auto missingVal = std::numeric_limits<int64_t>::min();
            // intsProto.Reserve(columnSize);
            intsProto.Resize(columnSize, missingVal);  // fill with missing values (0 for now
            std::span<int64_t> target(&intsProto[0], &intsProto[0]+columnSize);
            assert(&intsProto[columnSize-1] >= target.data() && &intsProto[columnSize-1] < target.data() + columnSize); // Ensure protobuf is contiguous (is this guaranteed or impl detail?)
            // TODO: directly filling in a shared array with different threads may have false-sharing cache performance issues.
            loadIntCol(qr.req, field, fieldType, collector, sortedIdx, sortedIdxRunLen, target, missingVal, tg);
          } // int field
          break;
          default:
            break;
        } // end switch on fieldType
      }  // for each field

      // fill in scores if requested
      if (returnScores) {
        auto& scoresProto = columnsProto["_score_"];
        auto& floatColProto = *scoresProto.mutable_col_f();
        auto& floatsProto = *floatColProto.mutable_v();
        // floatColProto.set_missing_val(-1.0f); // TODO
        floatsProto.Reserve(columnSize);
        for (int i=0; i<columnSize; i++) {
          floatsProto.Add(collector.topDocs[i].score);
        }
      }

      // To continue after all fields are loaded, we could have a callback that counts down and then calls a final callback
      // to launch the next task, or we could just use a nested task_group and call wait.
      if (tg != nullptr) {
        tg->wait();
      }
    }
  }

  // Message to load part of an integer column.  One reason for bundling info like this is that it can be passed
  // easily or captured by a lambda and assigned to a std::function without heap allocation.
  // EDIT: actually, TBB task_group.run() is a template function that eventually requests thread local pool allocation, so
  // the std::function limitation of heap allocation with over 2 pointers doesn't apply.
  /*
  struct IntColSeg {
    std::string_view field;
    TopDocsCollector& collector;
    std::span<uint8_t> sortedIdx;
    std::span<int64_t> target;
    int64_t missingVal;
  };
   */

  // TODO: abstract the iterator over the documents since we will have different ways of getting the ids.  Perhaps just a callable that returns a segdoc given an index?
  void loadIntCol(SearchEngine::Request& req, std::string_view field, FieldType& fieldType,
                  const TopDocsCollector& collector, const std::span<uint8_t> sortedIdx, const std::span<uint8_t> sortedIdxRunLen, std::span<int64_t> target, int64_t missingVal,
                  oneapi::tbb::task_group* tg)
  {
    auto& topDocs = collector.topDocs;
    int start = 0;
    // iterate over the segment runs
    while (start < sortedIdx.size()) {
      auto runlen = sortedIdxRunLen[start];
      auto segSpan = sortedIdx.subspan(start, runlen);
      task_group_run(tg, [this, &req, &field, &fieldType, &collector, segSpan, target, missingVal]() {
        loadIntColSeg(*req.reader, field, fieldType, collector, segSpan, target, missingVal);
      });
      start += runlen;
    }
  }

  void loadIntColSeg(IndexReader& reader, std::string_view field, FieldType& fieldType, const TopDocsCollector& collector, const std::span<uint8_t> sortedIdx, std::span<int64_t> target, int64_t missingVal) {
    // Hmm, we could also just pass in a segment and not the whole reader.
    auto segNum = collector.topDocs[sortedIdx[0]].doc.segment();
    auto& postingsReader = reader.segments()[segNum].postingsReader();
    auto poolGuard = MemPool::threadLocalPoolGuard();
    FieldReader fieldReader(poolGuard.pool(), postingsReader);
    bool found = fieldReader.seek(field);
    if (!found) {
      // field not found in this segment.  fill missing values.
      for (auto idx : sortedIdx) {
        target[idx] = missingVal;
      }
      return;
    }

    SegFieldInfo segFieldInfo;
    fieldReader.readFieldInfo(segFieldInfo);
    IntColReader intColReader(poolGuard.pool(), postingsReader, segFieldInfo);
    IntColReader::Iterator iter(intColReader);

    int32_t next = -1;
    for (auto idx : sortedIdx) {
      auto segdoc = collector.topDocs[sortedIdx[idx]].doc;
      assert(segdoc.segment() == segNum);
      int32_t docid = segdoc.docId();
      if (docid < next) {
        target[idx] = missingVal;
        continue;
      }

      if (docid > next) {
        next = iter.advance(docid);
      }

      if (docid == next) {
        target[idx] = iter.value();
      } else {
        target[idx] = missingVal;
      }
    }
  }

  void loadStoredFields() {
    // We need the spec of what fields to load.  A column-wise load would be more efficient, and
    // we should probably default to a column representation for returned results as well.
    // Unfortunately, filling in values in-place across multiple tasks would most likely lead to bad cache effects
    // via false-sharing.

    // If we wanted to do batches of less than 256, we could use byte indexes (plus an offset) into the sorted docs.

    // How to implement returning all fields?  We can check per-field if the number of fields is small, but
    // otherwise (maybe) add index info for what fields a document has?  Or maybe just for dynamic fields?
  }



  SearchEngine(const SearchEngine&) = delete;
  SearchEngine& operator=(const SearchEngine&) = delete;
};

} // namespace solux