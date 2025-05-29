#pragma once

#include <solux/query/ProtobufQueryParser.h>
#include <oneapi/tbb/flow_graph.h>
#include <solux/server/SoluxNode.h>
#include <solux/util/proto.h>
#include <ranges>
#include "protos/solux_types.pb.h"
#include "IndexReader.h"
#include "Collector.h"
#include "Facet.h"

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
      // set the response id to match the request id.
      proto.set_request_id(req.proto.request_id());
    }

    // Arena allocate a Response object.
    static Response* create(SearchEngine::Request& req, bool last=true) {
      // If this is the last message, just use the same arena as the response since they will have
      // the same lifetimes.
      auto* arena = last ? &req.arena : createArena();
      return google::protobuf::Arena::Create<Response>(arena, req, *arena, last);
    }

    // named columns, part of DocList or part of a FacetResult.
    using ColumnsType = google::protobuf::Map<std::string, solux::proto::Column>;
  };

  class QueryReq {
  public:
    SearchEngine::Request& req;
    Query::Context& qcontext;
    Query* query;
    Query::Weight* weight;
    int64_t topCount;  // maximum number of docs to return.
    std::vector<FacetReq*>* facets = nullptr;
    FacetDomain* dom = nullptr;  // the domain for the facet

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

    ~QueryReq() {
      auto* holder = collectorHolder.load(std::memory_order_relaxed);
      if (holder != nullptr && holder->heapAllocated) {
        delete holder;
      }
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
        bool allSegsMerged = (size_t(holder->segmentsMerged) == req.reader->segments().size());
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
      // [[unreachable]];
    }

    void collect(int32_t segnum) {
      CollectorHolder* holder = nullptr;
      {
        auto poolGuard = MemPool::threadLocalPoolGuard();
        auto scorer = weight->createScorer(poolGuard.pool(), qcontext.topReader.segments()[segnum]);

        // Wait until last moment to obtain collector in hopes of reusing an existing one.
        holder = getCollector();

        std::vector<bool>* currBitset = dom ? &dom->allMatches[segnum].docs : nullptr;
        if (currBitset) {
          currBitset->resize(req.reader->numDocs());
        }

        if (scorer != nullptr) {
          auto& collector = holder->collector;
          for (;;) {
            auto doc = scorer->next();
            if (doc == PostingsReader::END) {
              break;
            }
            if (currBitset) {
              (*currBitset)[doc] = true;
            }
            auto score = scorer->score();
            collector.collect(segnum, doc, score);
          }
        }
      }

      if (releaseCollector(holder)) {
        // we are done, so we can call the callback
        // we could use a nested task_group to wait until we are done here as well.
        callback(*this);
      }
    }

    void start(oneapi::tbb::task_group* tg) {
      for (int32_t i=0; i < (int32_t)req.reader->segments().size(); i++) {
        task_group_run(tg, [this, i]() {
          this->collect(i);
          if (facets != nullptr) {
            for (auto* facet : *facets) {
              facet->facetSeg(*dom, i);
            }
          }
        });
      }

      // handle special case of no segments.
      if (req.reader->segments().size() == 0) {
        // we are done, so we can call the callback
        callback(*this);
      }
    }
  };


  void submit(SearchEngine::Request& req, bool parallel = true) {
    try {
      std::optional<oneapi::tbb::task_group> stackTg;
      oneapi::tbb::task_group* tg;
      if (parallel && req.tg == nullptr) {
        req.tg = &stackTg.emplace();
      }
      tg = req.tg;
      submitBody(req);
      if (tg) {
        tg->wait();
      }
    } catch (std::exception& e) {
      LOG_ERROR("Unexpected exception: {}", e.what());
    }
  }

  void submitBody(SearchEngine::Request& req) {
    getResources(req);

    req.lastResponse = Response::create(req, true);

    // We probably want to parse all up-front in a single task to understand dependencies.

    solux::proto::SearchRequest& proto = req.proto;

    std::vector<QueryReq*> queryReqs;
    std::vector<FacetReq*> facetReqs;
    FacetDomain dom;

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
          auto facetField = facetReq.field();
          auto limit = 5; // default limit
          if (facetReq.has_limit()) {
            limit = facetReq.limit();
          }
          auto missing = facetReq.missing();
          //arena allocate FacetReq
          FacetReq* facet = google::protobuf::Arena::Create<IntFacetReq>(&req.arena, *req.reader, facetField, opKey, limit, missing);
          facetReqs.push_back(facet);
        } // end case
          break;
        default:
          // already handled, or ignoring for now
          break;
      } // end switch
    }
    if (!facetReqs.empty()) {
      queryReqs[0]->facets = &facetReqs;
      queryReqs[0]->dom = &dom;
      dom.allMatches.resize(req.reader->segments().size());
    }

    // launch all top-level queries
    for (auto qr : queryReqs) {
      qr->start(req.tg);
    }

    // do we need to special case when there we no queries or facets?
    if (req.tg != nullptr) {
      req.tg->wait();
    }

    for (auto* facet: facetReqs) {
      auto& response = *req.lastResponse;
      auto& searchResultProto = response.proto.mutable_ops()->operator[](facet->facetName);
      auto& facetResultProto = *searchResultProto.mutable_facet();
      facet->facetResult(facetResultProto);
    }

    // send back the final response
    req.reply(*req.lastResponse);
  }


  /// get needed resources such as the index reader and schema
  void getResources(SearchEngine::Request& req);


  // This is currently called after all segments have been collected for a TopN query to
  // fill out a DocList proto message.
  void fillQueryTopNResponse(QueryReq& qr) {
//    message DocList {
//       sint64 matches = 1;
//       float max_score = 2;
//       Columns columns = 3;
//       int64 offset = 4;
//       bool more = 5; // expect more results to be streamed back?
//    }

    // Grab the TopDocs from the collector and fill in the fields.  Search is done, so we don't need
    // the atomic at all here.
    auto* collectorHolder = qr.collectorHolder.load(std::memory_order_relaxed);

    if (collectorHolder == nullptr) {
      auto& response = *qr.req.lastResponse;
      auto& searchResultProto = response.proto.mutable_ops()->operator[](qr.name);
      auto& docListProto = *searchResultProto.mutable_docs();
      docListProto.set_matches(0);
      return;
    }

    auto& collector = collectorHolder->collector;
    collector.sort();
    auto numCollected = collector.size();
    int32_t maxBatchSize = qr.topDocsProto->batch_size();
    if (maxBatchSize <= 0) {
      maxBatchSize = 10;  // what should the default be?
    } else if (maxBatchSize > 256) {
      maxBatchSize = 256;
    }

    // starting offset into the topDocs list as specified by the request.
    int64_t offset = qr.topDocsProto->offset();
    auto batches = collector.scoreDocs() | std::views::drop(offset) | std::views::chunk(maxBatchSize);

    // we want to ensure we always enter the loop at least once, so we use iterators instead of range-based for.
    for (auto batchIter = batches.begin();; batchIter++) {
      if (batchIter == batches.end() && batches.size() > 0) {
        // we reached then end of the non-zero length list
        break;
      }

      bool lastResponse = batchIter == batches.end() || std::next(batchIter) == batches.end();
      auto& response = lastResponse ? *qr.req.lastResponse : *Response::create(qr.req, lastResponse);
      auto& searchResultProto = response.proto.mutable_ops()->operator[](qr.name);
      auto& docListProto = *searchResultProto.mutable_docs();
      docListProto.set_offset(offset);
      if (!lastResponse) {
        docListProto.set_more(true);
        response.proto.set_more(true);  // also set at the response level for easier client handling.
      }

      if (qr.topDocsProto->get_number()) {
        docListProto.set_matches(collector.totalHits());
      }

      // Should we somehow do auto-sizing of return messages?
      // We could load largest field first and cut it off after it gets big enough.  That can't really be
      // parallelized easily though.
      // We could also do it based on index data of the average column size.

      if (batchIter == batches.end()) {
        // no batch to process.
        break;
      }

      auto batch = *batchIter;
      auto segDocs = batch | std::views::transform([](auto& sd) { return sd.doc; });
      int columnSize = segDocs.size();

      // std::optional keeps constructor from being called if not needed.  We could also pool allocate it.
      std::optional<oneapi::tbb::task_group> loadColumnsTaskGroup;
      oneapi::tbb::task_group* tg = qr.req.tg ? &loadColumnsTaskGroup.emplace() : nullptr;

      bool returnScores = qr.topDocsProto->get_scores();

      // indirect sort the documents so we can access them in order of both segment and docid
      std::vector<uint8_t> sortedIdx(segDocs.size()); // uint8_t works for up to 256 docs.
      std::iota(sortedIdx.begin(), sortedIdx.end(), 0);
      // for small int lists, std::sort is faster than radix sort.
      std::ranges::sort(sortedIdx, [&segDocs](auto a, auto b) {
        return segDocs[a] < segDocs[b];
      });

      // calculate segment run lengths so they can be reused when retrieving each column.
      // we don't actually store the generated runs from the view since that would take 16x the memory (up to 4K)
      auto bySeg = sortedIdx
                   | std::views::chunk_by([&segDocs](auto a, auto b) {
        return segDocs[a].segment() == segDocs[b].segment();
      })
                   | std::views::transform([](const auto& run) { return (uint8_t)run.size(); });

      std::vector<uint8_t> segRunLength;
      std::ranges::copy(bySeg, std::back_inserter(segRunLength));

      auto& columnsProto = *docListProto.mutable_columns();

      // Let's look at the fields we should retrieve
      for (std::string_view field: qr.topDocsProto->fields()) {
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
            // TODO: how to decide if a column should be sparse?  Allow client to opt-in for sparse columns.
            loadIntCol(qr.req, field, fieldType, segDocs, sortedIdx, segRunLength, columnsProto, tg);
          } // int field
            break;
          case FieldType::Type::STRING: {
            loadStrCol(qr.req, field, fieldType, segDocs, sortedIdx, segRunLength, columnsProto, tg);
          } // string field
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
        for (int i = 0; i < columnSize; i++) {
          floatsProto.Add(collector.topDocs[offset + i].score);
        }
      }

      // To continue after all fields are loaded, we could have a callback that counts down and then calls a final callback
      // to launch the next task, or we could just use a nested task_group and call wait.
      // One advantage to waiting is that we can mempool allocate temporary stuff and then deallocate it again when wait returns.
      if (tg != nullptr) {
        tg->wait();
      }

      // if this is the last response, just return.  Otherwise, we need to send the response and continue.
      if (!lastResponse) {
        auto numBuffered = qr.req.reply(response);
        unused(numBuffered);
        // This code currently serializes the produce-batch, send-batch loop.
        // We could get better throughput by loading the fields for multiple batches at once.
        // TODO: we also need some flow control to limit the number of buffered responses.
        // Do we need to distinguish between buffered writes from *this* logical request vs others?
        // Probably not, except for the fact that we need at least *one* of the outstanding writes
        // to be ours so we can unblock when it's written.
        // Due to race conditions (callback being called *before* we decide we need to block), we should
        // probably block on the *next* reply, not the current one.
      }

      offset += columnSize;
    }  // end for
  }


  // We are guaranteed that the resources passed here will remain valid for any subtasks added to "tg" (i.e. the
  // caller waits on "tg" before releasing the resources).
  void loadIntCol(SearchEngine::Request& req, std::string_view field, FieldType& fieldType,
                   std::ranges::input_range auto& segDocs, std::span<uint8_t> sortedIdx, const std::span<uint8_t> segRunLength,
                   Response::ColumnsType& columnsProto, oneapi::tbb::task_group* tg)
  {
    auto& fieldCol = columnsProto[field];  // output Column in the protobuf
    //load a span for the single valued or multi valued target
    std::span<int64_t> starget; // single valued target
    std::span<solux::proto::ArrInt*> mtarget;  // multi-valued target
    auto columnSize = segDocs.size();

    if (!fieldType.multiValued()) {
      auto& intCol = *fieldCol.mutable_col_i();
      auto& intsProto = *intCol.mutable_v();
      // intCol.set_missing_val(0); // TODO.... get from schema? Set even if all values present?
      auto missingVal = std::numeric_limits<int64_t>::min();
      intsProto.Resize(columnSize, missingVal);  // fill with missing values
      starget = {&intsProto[0], &intsProto[0] + columnSize};
      // Ensure protobuf is contiguous (is this guaranteed or impl detail?)
      assert(&intsProto[columnSize - 1] >= starget.data() && &intsProto[columnSize - 1] < starget.data() +columnSize);
    } else {
      // multi-valued field type (even if only one value per doc currently)
      auto& intCol = *fieldCol.mutable_multi_i();
      auto& arrArrProto = *intCol.mutable_v();  // v is a repeated ArrInt
      arrArrProto.Reserve(columnSize);
      for (int i = 0; i < columnSize; i++) {
        arrArrProto.Add();
      }
      // arrArrProto is implemented as a vector<ArrInt*> under the covers (RepeatedPtrField), so our span
      // should be of pointers.
      solux::proto::ArrInt** arrstart = arrArrProto.mutable_data();
      auto** arrEnd = arrstart + columnSize;
      assert(&arrArrProto.Get(columnSize-1) == arrstart[columnSize-1]); // sanity check that arr is actually contiguous.
      mtarget = {arrstart, columnSize};
    }
    // iterate over the segment runs, loading values for each segment (possibly in a new task)
    int32_t start = 0;
    for(auto runlen : segRunLength) {
      auto idxSpan = sortedIdx.subspan(start, runlen);
      // capture by-value parameters by-value again since this method will return before the lambda is executed.
      // Don't specify a default capture, going across task boundaries should be very explicit.
      task_group_run(tg, [this, idxSpan, &req, field, &fieldType, &segDocs, starget, mtarget]() {
        // a view of the segdocs for a single segment, in ascending order.
        auto sortedSegDocs = idxSpan | std::views::transform([&segDocs](auto idx) { return segDocs[idx]; });
        auto segNum = sortedSegDocs[0].segment();
        // extract the docids from the sorted segdocs
        auto sortedDocs = sortedSegDocs | std::views::transform([](const auto& sd) { return sd.docId(); });

        auto& postingsReader = req.reader->segments()[segNum].postingsReader();
        auto poolGuard = MemPool::threadLocalPoolGuard();
        FieldReader fieldReader(poolGuard.pool(), postingsReader);
        bool found = fieldReader.seek(field);
        if (!found) {
          return;
        }
        SegFieldInfo segFieldInfo;
        fieldReader.readFieldInfo(segFieldInfo);

        if (!fieldType.multiValued()) {
          // callback handler to put values back in the correct slot.  The task lambda captured by value when necessary
          // already, so it's fine to capture by ref here.
          auto valHandler = [&](size_t idx, int32_t doc, int64_t val) {
            assert(segDocs[idxSpan[idx]].docId() == doc);
            // translate back to the original index
            starget[idxSpan[idx]] = val;
          };
          IntColReader::getSingleValues(poolGuard.pool(), postingsReader, segFieldInfo, sortedDocs, valHandler);
        } else {
          // multi-valued handling
          auto valHandler = [&](size_t idx, int32_t doc, int64_t val, int64_t valIdx, int64_t numVals) {
            assert(segDocs[idxSpan[idx]].docId() == doc && val > 0);
            solux::proto::ArrInt& target = *mtarget[idxSpan[idx]];
            if (valIdx == 0) {
              // first value for this doc.
              target.mutable_v()->Reserve(numVals);
            }
            target.mutable_v()->Add(val);
          };

          IntColReader::getValues(poolGuard.pool(), postingsReader, segFieldInfo, sortedDocs, valHandler);
        }
      });

      start += runlen;
    }
  }


  void loadStrCol(SearchEngine::Request& req, std::string_view field, FieldType& fieldType,
                  std::ranges::input_range auto& segDocs, std::span<uint8_t> sortedIdx, const std::span<uint8_t> segRunLength,
                  Response::ColumnsType& columnsProto, oneapi::tbb::task_group* tg)
  {
    auto columnSize = segDocs.size();
    auto& fieldCol = columnsProto[field];  // output Column in the protobuf
    std::span<std::string*> starget; // single valued target
    std::span<solux::proto::ArrStr*> mtarget;  // multi-valued target

    if (!fieldType.multiValued()) {
      auto& strCol = *fieldCol.mutable_col_s();
      auto& stringsProto = *strCol.mutable_v();
      stringsProto.Reserve(columnSize);
      std::string missingVal;
      // under the covers, the vector contains pointers, not elements (i.e. vector<std::string*>)
      for (int i = 0; i < columnSize; i++) {
        stringsProto.Add("");
      }
      auto* arrstart = stringsProto.mutable_data();
      starget = {arrstart, arrstart + columnSize};
    } else {
      // multi-valued field type (even if only one value per doc currently)
      auto& strCol = *fieldCol.mutable_multi_s();
      auto& arrArrProto = *strCol.mutable_v();  // v is a repeated ArrStr
      arrArrProto.Reserve(columnSize);
      for (int i = 0; i < columnSize; i++) {
        arrArrProto.Add();
      }
      // arrArrProto is implemented as a vector<ArrStr*> under the covers (RepeatedPtrField), so our span
      // should be of pointers.
      solux::proto::ArrStr** arrstart = arrArrProto.mutable_data();
      auto** arrEnd = arrstart + columnSize;
      assert(&arrArrProto.Get(columnSize-1) == arrstart[columnSize-1]); // sanity check that arr is actually contiguous.
      mtarget = {arrstart, columnSize};
    }


    // iterate over the segment runs, loading values for each segment (possibly in a new task)
    // much of this code will be the same as loading the IntCol, but we are retrieving ords
    // and need to look them up.
    int32_t start = 0;
    for(auto runlen : segRunLength) {
      auto idxSpan = sortedIdx.subspan(start, runlen);
      // capture by-value parameters by-value again since this method will return before the lambda is executed.
      // also capture anything on the stack in this function by-value.
      task_group_run(tg, [this, idxSpan, &req, field, &fieldType, &segDocs, starget, mtarget]() {
        // a view of the segdocs for a single segment, in ascending order.
        auto sortedSegDocs = idxSpan | std::views::transform([&segDocs](auto idx) { return segDocs[idx]; });
        auto segNum = sortedSegDocs[0].segment();
        // extract the docids from the sorted segdocs
        auto sortedDocs = sortedSegDocs | std::views::transform([](const auto& sd) { return sd.docId(); });

        auto& postingsReader = req.reader->segments()[segNum].postingsReader();
        auto poolGuard = MemPool::threadLocalPoolGuard();
        FieldReader fieldReader(poolGuard.pool(), postingsReader);
        bool found = fieldReader.seek(field);
        if (!found) {
          return;
        }
        SegFieldInfo segFieldInfo;
        fieldReader.readFieldInfo(segFieldInfo);
        // we need the terms enum to lookup the string value for each ord
        TermsEnum tenum(poolGuard.pool(), postingsReader, segFieldInfo);

        if (!fieldType.multiValued()) {
          // callback handler to convert ord to string and to put values back in the correct slot.
          auto valHandler = [&](size_t idx, int32_t doc, int64_t val) {
            assert(segDocs[idxSpan[idx]].docId() == doc && val > 0);
            // translate back to the original index
            // TODO: we could introduce a cache here to avoid repeated lookups for the same term.
            // it could be map<int64_t, string_view or string*> since the protobuf values won't be moving around.
            tenum.seekOrd((int32_t) val - 1);  // term ords are 0 based.
            // LHS is a std::string&, so this makes a copy (which we need to do since tenum.term() will soon be invalidated.)
            *starget[idxSpan[idx]] = (std::string_view) tenum.term();
          };

          IntColReader::getSingleValues(poolGuard.pool(), postingsReader, segFieldInfo, sortedDocs, valHandler);
        } else {
          // multi-valued handling
          auto valHandler = [&](size_t idx, int32_t doc, int64_t val, int64_t valIdx, int64_t numVals) {
            assert(segDocs[idxSpan[idx]].docId() == doc && val > 0);
            solux::proto::ArrStr& target = *mtarget[idxSpan[idx]];
            if (valIdx == 0) {
              // first value for this doc.
              target.mutable_v()->Reserve(numVals);
            }
            tenum.seekOrd((int32_t) val - 1);  // term ords are 0 based.
            auto v = (std::string_view) tenum.term();
            auto* strProto = target.mutable_v()->Add();
            *strProto = v;
          };

          IntColReader::getValues(poolGuard.pool(), postingsReader, segFieldInfo, sortedDocs, valHandler);
        }
      });

      start += runlen;
    }
  }


  void loadStoredFields() {
    // We need the spec of what fields to load.  A column-wise load would be more efficient, and
    // we should probably default to a column representation for returned results as well.
    // Unfortunately, filling in values in-place across multiple tasks would most likely lead to bad cache effects
    // via false-sharing.

    // How to implement returning all fields?  We can check per-field if the number of fields is small, but
    // otherwise (maybe) add index info for what fields a document has?  Or maybe just for dynamic fields?
  }



  SearchEngine(const SearchEngine&) = delete;
  SearchEngine& operator=(const SearchEngine&) = delete;
};

} // namespace solux