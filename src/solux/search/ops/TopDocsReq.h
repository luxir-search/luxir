#pragma once

#include <cstring>
#include "SearchOp.h"
#include "solux/query/AllQuery.h"
#include "solux/query/Query.h"
#include "solux/reader/IntColReader.h"
#include "solux/reader/StoredFieldsReader.h"
#include "solux/reader/StrColReader.h"
#include "solux/search/Collector.h"
#include "solux/search/FieldSortCollector.h"
#include "solux/search/SortField.h"
#include "solux/search/SearchRequest.h"
#include "solux/util/AtomicMerger.h"

namespace solux {


class TopDocsReq : public SearchOp {
protected:

public:
  const solux::proto::TopDocs& topDocsProto;  // the relevant part of the protobuf request
  Query::Context& qcontext;
  Query* query;
  Query::Weight* weight;
  int64_t topCount; // maximum number of docs to return.
  std::span<std::pair<std::string_view, Query*>> filters;
  std::span<Query::Weight*> filterWeights;
  std::vector<SortField> sortFields;
  bool useFieldSort = false;


  class Calc : public SearchOp::Calculator {
  public:
    solux::proto::Val* getTargetForSub(solux::proto::SearchResponse* searchResponse, Calculator* sub) override {
      auto* ourVal = parent->getTargetForSub(searchResponse, this);
      // the Val should either be unset, or have a DocList
      assert(
        ourVal != nullptr && (ourVal->kind_case() == solux::proto::Val::kDocs
          || ourVal->kind_case() == solux::proto::Val::KIND_NOT_SET));
      return &(*ourVal->mutable_docs()->mutable_ops())[sub->getOp().name];
    }

    Calc(TopDocsReq& op, Calculator* parent) : SearchOp::Calculator(op, parent, -1, -1), collectorMerger(nullptr, nullptr) {

      collectorMerger.creator = [&op]() -> MergeableCollector* {
        return new MergeableCollector(op.topCount, op.useFieldSort, op.sortFields, op.req.reader.get());
      };
      collectorMerger.destroyer = [](MergeableCollector* data) {
        delete data;
      };

      if (op.subOps.size() > 0) {
        output.resize(op.req.reader->segments().size());
        subCalcs.reserve(op.subOps.size());
        for (auto& [key, subOp] : op.subOps) {
          auto* subCalc = subOp->createCalculator(this, -1);
          subCalcs.emplace_back(subCalc);
        }
      }
    }

    TopDocsReq& thisOp() {
      return static_cast<TopDocsReq&>(op);
    }



    class MergeableCollector : public MergeableData {
    public:
      // QueryReq* queryReq;  // the query request that this collector is for
      std::unique_ptr<TopDocsCollector> scoreCollector;
      std::unique_ptr<FieldSortCollector> fieldCollector;
      bool useFieldSort;

      MergeableCollector(size_t topCount, bool useFieldSort, const std::vector<SortField>& sortFields, IndexReader* reader = nullptr) 
        : useFieldSort(useFieldSort) {
        if (!useFieldSort) {
          scoreCollector = std::make_unique<TopDocsCollector>(topCount);
        } else {
          std::unique_ptr<FieldComparator> comparator;
          if (sortFields.size() == 1) {
            // For single field sort, create the comparator directly
            comparator = sortFields[0].createComparator(topCount, reader);
          } else {
            // For multiple fields, use MultiFieldComparator
            comparator = std::make_unique<MultiFieldComparator>(sortFields, topCount, reader);
          }
          fieldCollector = std::make_unique<FieldSortCollector>(topCount, std::move(comparator));
        }
      }

      static MergeableCollector* merge(MergeableCollector* a, MergeableCollector* b) {
        // merge the smaller collector into the larger collector, or if both the same size, merge
        // the less competitive collector into the more competitive collector.
        if (a->useFieldSort) {
          if (a->fieldCollector->size() < b->fieldCollector->size()) {
            std::swap(a, b);
          }
          a->fieldCollector->merge(*b->fieldCollector);
        } else {
          if (a->scoreCollector->size() < b->scoreCollector->size()
            || a->scoreCollector->minCompetitiveVal < b->scoreCollector->minCompetitiveVal) {
            std::swap(a, b);
          }
          a->scoreCollector->merge(*b->scoreCollector);
        }
        return a;
      }
    };

    AtomicMerger<MergeableCollector> collectorMerger;


    // produced domains for subOps.
    // TODO: how to avoid having 2 allocations per set, one for the DocSet and one for the memory in the DocSet?
    // Arena allocate?
    // We could have std::variant of DocSet.
    std::vector<std::unique_ptr<DocSet>> output;
    std::vector<std::unique_ptr<Calculator>> subCalcs;

    void calc(oneapi::tbb::task_group* tg, int32_t segnum, solux::DocSet* domain) override {
      task_group_run(tg, [this, tg, segnum, domain]() {
        doCalc(tg, segnum, domain);
      });
    }

    void doCalc(oneapi::tbb::task_group* tg, int32_t segnum, solux::DocSet* domain) {
      auto& op = thisOp();

      if (segnum < 0) {
        // special case for empty index reader, we are done.
        // TODO: pass along to sub-calculators?
        doneCollecting();
        return;
      }

      // If we have subcalcs and if we determine that we are matching everything, then we can skip collecting
      // a new domain and just use the existing one.
      // TODO: put a type field on the query and replace this dynamic cast.
      bool matchEverything = (dynamic_cast<AllQuery*>(op.query) != nullptr) && thisOp().filters.empty();
;
      MergeableCollector* data = nullptr;
      int64_t numSegs = (int64_t)op.req.reader->segments().size();

      {
        auto poolGuard = MemPool::threadLocalPoolGuard();
        auto& seg = op.qcontext.topReader.segments()[segnum];
        auto* scorer = op.weight->createScorer(poolGuard.pool(), seg);

        // Wait until last moment to obtain collector in hopes of reusing an existing one.
        data = collectorMerger.obtain();

        std::optional<DocSetBuilder> builder;
        if (output.size() > 0) {
          // check if the query is a match-all-docs query with a dynamic cast
          matchEverything = (dynamic_cast<AllQuery*>(op.query) != nullptr) && thisOp().filters.empty();
          if (!matchEverything) {
            builder.emplace(seg.maxDoc());
          }
        }


        if (scorer != nullptr) {
          DocSet* filter = domain;
          std::unique_ptr<DocSet> newDomain;;
          if (!thisOp().filterWeights.empty()) {
            std::vector<std::unique_ptr<DocSet>> filters;
            std::vector<DocSet*> filterPtrs;
            for (auto weight : thisOp().filterWeights) {
              filters.push_back(thisOp().getDocSet(*weight, segnum));
              filterPtrs.push_back(filters.back().get());
            }
            if (domain) {
              filterPtrs.emplace_back(domain);
            }
            newDomain = DocSet::intersect(filterPtrs);
            filter = newDomain.get();
          }
          if (filter == nullptr || filter->type == DocSet::BITSET) {
            BitDocSet* bitDocs = (BitDocSet*)filter;
            auto* domainBits = bitDocs ? &bitDocs->bits() : nullptr;
            if (data->useFieldSort) {
              data->fieldCollector->setSegment(segnum, &seg.postingsReader());
              auto& collector = *data->fieldCollector;
              for (;;) {
                auto doc = scorer->next();
                if (doc == PostingsReader::END) {
                  break;
                }
                if (domainBits && !domainBits->get(doc)) {
                  continue;
                }
                if (builder.has_value()) {
                  builder->add(doc);
                }
                auto score = scorer->score();
                collector.collect(segnum, doc, score);
              }
            } else {
              auto& collector = *data->scoreCollector;
              for (;;) {
                auto doc = scorer->next();
                if (doc == PostingsReader::END) {
                  break;
                }
                if (domainBits && !domainBits->get(doc)) {
                  continue;
                }
                if (builder.has_value()) {
                  builder->add(doc);
                }
                auto score = scorer->score();
                collector.collect(segnum, doc, score);
              }
            }
          } else {
            assert(filter->type == DocSet::ARRAY);
            ArrDocSet* arrDocs = (ArrDocSet*)filter;
            if (data->useFieldSort) {
              data->fieldCollector->setSegment(segnum, &seg.postingsReader());
              auto& collector = *data->fieldCollector;
              for (auto doc : arrDocs->docs()) {
                if (scorer->docId() < doc) {
                  scorer->advance(doc);
                }
                if (scorer->docId() != doc) {
                  continue; // this doc does not match the query
                }
                if (builder.has_value()) {
                  builder->add(doc);
                }
                auto score = scorer->score();
                collector.collect(segnum, doc, score);
              }
            } else {
              auto& collector = *data->scoreCollector;
              for (auto doc : arrDocs->docs()) {
                if (scorer->docId() < doc) {
                  scorer->advance(doc);
                }
                if (scorer->docId() != doc) {
                  continue; // this doc does not match the query
                }
                if (builder.has_value()) {
                  builder->add(doc);
                }
                auto score = scorer->score();
                collector.collect(segnum, doc, score);
              }
            }
          }
        }
        if (builder.has_value()) {
          output[segnum] = std::move(builder->build());
        }
      }
      // For maximum parallelism, we want to launch sub-tasks that depend on matching documents
      // as soon as we have that set.  Releasing the collector below could end up
      // doing a substantial amount of work.

      // since tasks are executed on a stack, push sub-calculators in reverse order.
      // directly calling a sub-calculator would be beneficial since the domain
      // we just calculated will still be in cache.
      // The downside is that it could delay loading stored fields.
      // We could launch fillQueryTopNResponse as a sub-task and then
      // launch the sub-calculators in parallel after that.
      for (int i = subCalcs.size() - 1; i >= 0; i--) {
        // TODO: launch sub-calculators in parallel (except for the first one).
        auto* newDomain = matchEverything? domain : output[segnum].get();
        subCalcs[i]->calc(tg, segnum, newDomain);
      }

      // Releasing the collector as soon as possible can save merging work.
      // On the other hand, it could delay the sub-calculators and spoil
      // the domain (which should be in the CPU cache).
      auto count = collectorMerger.release(data);
      if (count == numSegs) {
        // we are done, so we can call the callback
        // we could use a nested task_group to wait until we are done here as well.

        // If there are sub-calculators that can run, lanch a separate task
        // for field loading.
        auto* tgFieldLoad = subCalcs.size() > 0 ? tg : nullptr;
        task_group_run(tgFieldLoad, [this]() {doneCollecting();});
      }




    }

    void doneCollecting() {
      thisOp().fillQueryTopNResponse(*this);
      // TODO: figure out what state we can dump before and after this call.
    };
  };


  //  req, *qcontext, query, limit
  // NOTE: keep this constructor nothrow.  TopDocsReq is created via
  // google::protobuf::Arena::Create<TopDocsReq>, which registers
  // ~TopDocsReq() with the arena *before* the body runs.  A throw mid-ctor
  // leaves a half-constructed object scheduled for cleanup, and
  // ~TopDocsReq() crashes on uninitialized members during arena reset.
  // All work that can throw (Weight construction, schema lookups for sort
  // fields) goes in init(), which runs after the object is fully built.
  TopDocsReq(SearchRequest& req, std::string_view name, const proto::TopDocs& topDocsProto, Query::Context& qcontext, Query* query, int64_t topCount,
    std::span<std::pair<std::string_view, Query*>> filters)
    : SearchOp(req, name), topDocsProto(topDocsProto), qcontext(qcontext), query(query),
      weight(nullptr), topCount(topCount), filters(filters) {
  }

  // Build sort fields, the main weight, and the filter weights here rather
  // than in the ctor so that throwing paths (schema field-not-found, kNN
  // dim mismatch, etc.) propagate cleanly out of submitBody.  See ctor
  // comment for the arena-cleanup hazard.
  void init() override {
    SearchOp::init();

    // Sort fields can throw if the schema doesn't have the field.
    if (topDocsProto.sorts_size() > 0) {
      useFieldSort = true;
      // Static instances of special field types
      static ScoreFieldType scoreType;
      static DocFieldType docType;

      for (const auto& sortSpec : topDocsProto.sorts()) {
        SortField::SortOrder order = sortSpec.dir() == proto::SortSpec::DESC ?
          SortField::DESC : SortField::ASC;
        FieldComparator::MissingValue missing = FieldComparator::MISSING_LAST;

        if (sortSpec.field() == "_score_") {
          sortFields.emplace_back(sortSpec.field(), scoreType, order, missing);
        } else if (sortSpec.field() == "_docid_") {
          sortFields.emplace_back(sortSpec.field(), docType, order, missing);
        } else {
          auto fieldTypePtr = req.schema->getFieldTypeEx(sortSpec.field());
          if (!fieldTypePtr) {
            throw std::runtime_error(std::string("Field not found in schema: ") + std::string(sortSpec.field()));
          }
          sortFields.emplace_back(sortSpec.field(), *fieldTypePtr, order, missing);
        }
      }
    }

    // Weight ctors run validation that can throw (e.g. KnnQuery dim check).
    weight = query->createWeight(qcontext);
    if (filters.size() > 0) {
      filterWeights = req.requestPool.make_span<Query::Weight*>(filters.size());
      for (size_t i = 0; i < filters.size(); i++) {
        filterWeights[i] = filters[i].second->createWeight(qcontext);
      }
    }
  }

  Calculator* createCalculator(Calculator* parent, int64_t slot = -1, int64_t numSlots = -1) override {
    return new Calc(*this, parent);
  }



  // This is currently called after all segments have been collected for a TopN query to
  // fill out a DocList proto message.
  // It currently blocks on it's own internal task_group until all results have been filled in.
  // and sends multiple streaming responses back to the client (all except the last one).
  void fillQueryTopNResponse(TopDocsReq::Calc& calc) {

    // auto& qr = calc.thisOp();
    auto& qr = *this;

    // Grab the TopDocs from the collector and fill in the fields.
    auto* mergeableCollector = calc.collectorMerger.getData();  // search is done, so it's safe to access this now.

    if (mergeableCollector == nullptr) {
      auto& searchResultProto = *calc.getTarget(nullptr);
      auto& docListProto = *searchResultProto.mutable_docs();
      docListProto.set_matches(0);
      return;
    }

    std::span<TopDocsCollector::ScoreDoc> scoreDocs;
    std::span<FieldSortCollector::SortDoc> sortDocs;
    int64_t totalHits = 0;
    int64_t numCollected = 0;
    
    if (mergeableCollector->useFieldSort) {
      auto& collector = *mergeableCollector->fieldCollector;
      totalHits = collector.totalHits();
      sortDocs = collector.sort();
      numCollected = sortDocs.size();
    } else {
      auto& collector = *mergeableCollector->scoreCollector;
      totalHits = collector.totalHits();
      scoreDocs = collector.sort();
      numCollected = scoreDocs.size();
    }
    unused(numCollected);
    int32_t maxBatchSize = qr.topDocsProto.batch_size();
    if (maxBatchSize <= 0) {
      maxBatchSize = 100;  // what should the default be?
    } else if (maxBatchSize > 256) {
      maxBatchSize = 256;
    }

    // starting offset into the topDocs list as specified by the request.
    int64_t offset = qr.topDocsProto.offset();
    
    // Create batches based on collector type
    // We need to handle the two collector types separately due to different doc types

    // Process documents in batches
    // If no documents were collected, we still need to send an empty response
    int64_t totalBatches = numCollected > 0 ? numCollected : 1;
    for (int64_t batchStart = 0; batchStart < totalBatches; batchStart += maxBatchSize) {
      int64_t batchEnd = std::min(batchStart + maxBatchSize, numCollected);
      int64_t batchSize = batchEnd - batchStart;

      bool lastResponse = batchEnd >= numCollected;
      auto& response = lastResponse ? *qr.req.lastResponse : *SearchResponse::create(qr.req, lastResponse);
      auto& searchResultProto = *calc.getTarget(&response.proto);
      // auto& searchResultProto = response.proto.mutable_ops()->operator[](qr.name);
      auto& docListProto = *searchResultProto.mutable_docs();
      docListProto.set_offset(offset + batchStart);
      if (!lastResponse) {
        docListProto.set_more(true);
        response.proto.set_more(true);  // also set at the response level for easier client handling.
      }

      if (qr.topDocsProto.get_number()) {
        docListProto.set_matches(totalHits);
      }

      // Should we somehow do auto-sizing of return messages?
      // We could load largest field first and cut it off after it gets big enough.  That can't really be
      // parallelized easily though.
      // We could also do it based on index data of the average column size.

      // Create a vector of segdocs from the appropriate collector
      std::vector<segdoc> batchSegDocs;
      
      if (numCollected > 0) {
        batchSegDocs.reserve(batchSize);
        
        if (mergeableCollector->useFieldSort) {
          for (int64_t i = batchStart; i < batchEnd; i++) {
            batchSegDocs.push_back(sortDocs[i].doc);
          }
        } else {
          for (int64_t i = batchStart; i < batchEnd; i++) {
            batchSegDocs.push_back(scoreDocs[i].doc);
          }
        }
      }
      
      auto segDocs = std::span(batchSegDocs);
      int columnSize = segDocs.size();
      

      // std::optional keeps constructor from being called if not needed.  We could also pool allocate it.
      std::optional<oneapi::tbb::task_group> loadColumnsTaskGroup;
      oneapi::tbb::task_group* tg = qr.req.tg ? &loadColumnsTaskGroup.emplace() : nullptr;

      bool returnScores = qr.topDocsProto.get_scores();

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

      // Stored-field retrieval is grouped by resource so that multiple stored
      // fields sharing one resource decompress each chunk only once.
      boost::unordered_flat_map<std::string_view, std::vector<StoredReq>,
                                PackedTermHash, PackedTermEqual> storedByResource;

      // Let's look at the fields we should retrieve
      for (std::string_view field: qr.topDocsProto.fields()) {
        // should we allow _scores_ as a field name?
        if (field == "_score_") {
          // TODO: if we are going to allow this, we would need to check earlier int he code to make sure we are recording scores.
          returnScores = true;
          continue;
        }

        // look up the field in the schema
        auto& fieldType = *qr.req.schema->getFieldTypeEx(field);

        // Collect any STORED field (column or not) into the candidate map;
        // the per-resource strategy is decided after the loop.  Rationale:
        // if some field in a resource is stored-only we'll decompress the
        // chunk anyway, in which case pulling column-backed peer fields out
        // of the same chunk is cheaper than doing separate column lookups.
        auto recordStoredReq = [&](std::string_view resourceName) {
          std::span<std::string*> starget;
          std::span<solux::proto::ArrStr*> mtarget;
          allocStringColumn(columnsProto[field], columnSize, fieldType.multiValued(), starget, mtarget);
          storedByResource[resourceName].push_back(
              {field, &fieldType, fieldType.multiValued(), starget, mtarget});
        };

        switch (fieldType.type()) {
          case FieldType::Type::INT: {
            // TODO: how to decide if a column should be sparse?  Allow client to opt-in for sparse columns.
            loadIntCol(qr.req, field, fieldType, segDocs, sortedIdx, segRunLength, columnsProto, tg);
            break;
          } // int field
          case FieldType::Type::ID:
          case FieldType::Type::STRING: {
            if (fieldType.isStored()) {
              recordStoredReq(fieldType.storedResource_);
            } else if (fieldType.hasColumn()) {
              loadStrCol(qr.req, field, fieldType, segDocs, sortedIdx, segRunLength, columnsProto, tg);
            }
            break;
          } // string / id field
          case FieldType::Type::TEXT: {
            if (fieldType.isStored()) {
              recordStoredReq(fieldType.storedResource_);
            }
            break;
          }
          case FieldType::Type::VECTOR: {
            loadVectorCol(qr.req, field, fieldType, segDocs, sortedIdx, segRunLength, columnsProto, tg);
            break;
          }
          default:
            break;
        } // end switch on fieldType
      }  // for each field

      // Per-resource strategy.  If any field in the group is stored-only we
      // decompress the resource's chunks regardless, so hand the whole
      // group to loadStoredFields and let it read every field from the
      // chunk.  If all fields in the group have columns available, skip
      // stored-fields entirely and read from columns — faster, no
      // decompression.
      for (auto& [resourceName, reqs] : storedByResource) {
        bool anyStoredOnly = std::ranges::any_of(reqs, [](const StoredReq& r) {
          return !r.fieldType->hasColumn();
        });
        if (anyStoredOnly) {
          loadStoredFields(qr.req, resourceName, std::move(reqs),
                           segDocs, sortedIdx, segRunLength, tg);
        } else {
          for (auto& r : reqs) {
            loadStrColWithTargets(qr.req, r.fieldName, *r.fieldType, r.starget, r.mtarget,
                                  segDocs, sortedIdx, segRunLength, tg);
          }
        }
      }

      // fill in scores if requested
      if (returnScores) {
        auto& scoresProto = columnsProto["_score_"];
        auto& floatColProto = *scoresProto.mutable_col_f();
        auto& floatsProto = *floatColProto.mutable_v();
        // floatColProto.set_missing_val(-1.0f); // TODO
        floatsProto.Reserve(columnSize);
        if (mergeableCollector->useFieldSort) {
          for (int i = 0; i < columnSize; i++) {
            floatsProto.Add(sortDocs[batchStart + i].score);
          }
        } else {
          for (int i = 0; i < columnSize; i++) {
            floatsProto.Add(scoreDocs[batchStart + i].score);
          }
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

    }  // end for
  }


  // We are guaranteed that the resources passed here will remain valid for any subtasks added to "tg" (i.e. the
  // caller waits on "tg" before releasing the resources).
  void loadIntCol(SearchRequest& req, std::string_view field, FieldType& fieldType,
                   std::ranges::input_range auto& segDocs, std::span<uint8_t> sortedIdx, const std::span<uint8_t> segRunLength,
                   SearchResponse::ColumnsType& columnsProto, oneapi::tbb::task_group* tg)
  {
    auto& fieldCol = columnsProto[field];  // output Column in the protobuf
    //load a span for the single valued or multi valued target
    std::span<int64_t> starget; // single valued target
    std::span<solux::proto::ArrInt*> mtarget;  // multi-valued target
    auto columnSize = segDocs.size();
    
    // If no documents, skip this field
    if (columnSize == 0) {
      return;
    }

    if (!fieldType.multiValued()) {
      auto& intCol = *fieldCol.mutable_col_i();
      auto& intsProto = *intCol.mutable_v();
      // intCol.set_missing_val(0); // TODO.... get from schema? Set even if all values present?
      auto missingVal = std::numeric_limits<int64_t>::min();
      intsProto.Resize(columnSize, missingVal);  // fill with missing values
      starget = {intsProto.mutable_data(), (size_t)columnSize};
      // Ensure protobuf is contiguous (is this guaranteed or impl detail?)
      assert(&intsProto[columnSize - 1] >= starget.data() && &intsProto[columnSize - 1] < starget.data() +columnSize);
    } else {
      // multi-valued field type (even if only one value per doc currently)
      auto& intCol = *fieldCol.mutable_multi_i();
      auto& arrArrProto = *intCol.mutable_v();  // v is a repeated ArrInt
      arrArrProto.Reserve(columnSize);
      for (auto i = 0u; i < columnSize; i++) {
        arrArrProto.Add();
      }
      // arrArrProto is implemented as a vector<ArrInt*> under the covers (RepeatedPtrField), so our span
      // should be of pointers.
      solux::proto::ArrInt** arrstart = arrArrProto.mutable_data();
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
        unused(this);
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


  void loadStrCol(SearchRequest& req, std::string_view field, FieldType& fieldType,
                  std::ranges::input_range auto& segDocs, std::span<uint8_t> sortedIdx, const std::span<uint8_t> segRunLength,
                  SearchResponse::ColumnsType& columnsProto, oneapi::tbb::task_group* tg)
  {
    auto columnSize = segDocs.size();
    if (columnSize == 0) return;
    std::span<std::string*> starget;
    std::span<solux::proto::ArrStr*> mtarget;
    allocStringColumn(columnsProto[field], columnSize, fieldType.multiValued(), starget, mtarget);
    loadStrColWithTargets(req, field, fieldType, starget, mtarget, segDocs, sortedIdx, segRunLength, tg);
  }

  // Per-segment body of loadStrCol.  Reads the field's column for one
  // segment and writes values into starget (single) or mtarget (multi).
  // No-op if the field isn't in the segment.  Extracted so the stored-fields
  // path can fall back here when a segment predates the STORED flag.
  void loadStrColForSegment(SearchRequest& req, std::string_view field, FieldType& fieldType,
                            std::span<uint8_t> idxSpan, std::ranges::input_range auto& segDocs,
                            std::span<std::string*> starget, std::span<solux::proto::ArrStr*> mtarget)
  {
    auto sortedSegDocs = idxSpan | std::views::transform([&segDocs](auto idx) { return segDocs[idx]; });
    auto segNum = sortedSegDocs[0].segment();
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

    bool isIndexedString = (segFieldInfo.flags & FieldType::INDEX_DOCS) != 0;

    if (!fieldType.multiValued()) {
      if (isIndexedString) {
        TermsEnum tenum(poolGuard.pool(), postingsReader, segFieldInfo);
        auto valHandler = [&](size_t idx, int32_t doc, int64_t val) {
          assert(segDocs[idxSpan[idx]].docId() == doc && val > 0);
          tenum.seekOrd((int32_t) val - 1);
          *starget[idxSpan[idx]] = (std::string_view) tenum.term();
        };
        IntColReader::getSingleValues(poolGuard.pool(), postingsReader, segFieldInfo, sortedDocs, valHandler);
      } else {
        auto valHandler = [&](size_t idx, int32_t doc, std::string_view val) {
          assert(segDocs[idxSpan[idx]].docId() == doc);
          *starget[idxSpan[idx]] = std::string(val);
        };
        StrColReader::getValues(poolGuard.pool(), postingsReader, segFieldInfo, sortedDocs, valHandler);
      }
    } else {
      if (isIndexedString) {
        TermsEnum tenum(poolGuard.pool(), postingsReader, segFieldInfo);
        auto valHandler = [&](size_t idx, int32_t doc, int64_t val, int64_t valIdx, int64_t numVals) {
          assert(segDocs[idxSpan[idx]].docId() == doc && val > 0);
          solux::proto::ArrStr& target = *mtarget[idxSpan[idx]];
          if (valIdx == 0) target.mutable_v()->Reserve(numVals);
          tenum.seekOrd((int32_t) val - 1);
          auto v = (std::string_view) tenum.term();
          auto* strProto = target.mutable_v()->Add();
          *strProto = v;
        };
        IntColReader::getValues(poolGuard.pool(), postingsReader, segFieldInfo, sortedDocs, valHandler);
      } else {
        auto valHandler = [&](size_t idx, int32_t doc, std::string_view val, int64_t valIdx, int64_t numVals) {
          assert(segDocs[idxSpan[idx]].docId() == doc);
          solux::proto::ArrStr& target = *mtarget[idxSpan[idx]];
          if (valIdx == 0) target.mutable_v()->Reserve(numVals);
          auto* strProto = target.mutable_v()->Add();
          *strProto = std::string(val);
        };
        StrColReader::getMultiValues(poolGuard.pool(), postingsReader, segFieldInfo, sortedDocs, valHandler);
      }
    }
  }

  // One stored-field retrieval request.  Collected per-field during dispatch
  // then grouped by resource so each (resource, segment) pair is served by a
  // single StoredFieldsReader — one chunk decompression covers all fields
  // sharing the resource.
  struct StoredReq {
    std::string_view fieldName;
    FieldType* fieldType;
    // Exactly one of starget / mtarget is populated (selected by multi flag).
    bool multi;
    std::span<std::string*> starget;
    std::span<solux::proto::ArrStr*> mtarget;
  };

  // Allocate the output Column (col_s or multi_s) for a TEXT/STRING field
  // and return spans into its internal storage.  Used by both column
  // retrieval (loadStrCol) and stored retrieval — the output proto shape is
  // identical in both cases.  Leaves spans empty when columnSize is 0.
  void allocStringColumn(solux::proto::Column& fieldCol, size_t columnSize, bool multi,
                         std::span<std::string*>& starget,
                         std::span<solux::proto::ArrStr*>& mtarget)
  {
    if (columnSize == 0) return;
    if (!multi) {
      auto& strCol = *fieldCol.mutable_col_s();
      auto& stringsProto = *strCol.mutable_v();
      stringsProto.Reserve(columnSize);
      for (auto i = 0u; i < columnSize; i++) stringsProto.Add("");
      starget = {stringsProto.mutable_data(), (size_t)columnSize};
    } else {
      auto& strCol = *fieldCol.mutable_multi_s();
      auto& arrArrProto = *strCol.mutable_v();
      arrArrProto.Reserve(columnSize);
      for (auto i = 0u; i < columnSize; i++) arrArrProto.Add();
      mtarget = {arrArrProto.mutable_data(), columnSize};
    }
  }

  // loadStrCol when the output spans are already allocated (by the caller's
  // earlier allocStringColumn).  Dispatches one per-segment task per run.
  void loadStrColWithTargets(SearchRequest& req, std::string_view field, FieldType& fieldType,
                             std::span<std::string*> starget, std::span<solux::proto::ArrStr*> mtarget,
                             std::ranges::input_range auto& segDocs, std::span<uint8_t> sortedIdx,
                             const std::span<uint8_t> segRunLength, oneapi::tbb::task_group* tg)
  {
    int32_t start = 0;
    for (auto runlen : segRunLength) {
      auto idxSpan = sortedIdx.subspan(start, runlen);
      task_group_run(tg, [this, idxSpan, &req, field, &fieldType, &segDocs, starget, mtarget]() {
        loadStrColForSegment(req, field, fieldType, idxSpan, segDocs, starget, mtarget);
      });
      start += runlen;
    }
  }

  // Decode a VECTOR column's bytes back to floats in the response.
  //
  // Single-valued output: Column.col_vec — one Vector per doc; missing docs
  // leave the slot's kind oneof unset.
  // Multi-valued output:  Column.multi_vec — one ArrVector per doc; missing
  // docs (or docs that indexed an empty list) leave an empty ArrVector.
  void loadVectorCol(SearchRequest& req, std::string_view field, FieldType& fieldType,
                     std::ranges::input_range auto& segDocs, std::span<uint8_t> sortedIdx,
                     const std::span<uint8_t> segRunLength,
                     SearchResponse::ColumnsType& columnsProto, oneapi::tbb::task_group* tg)
  {
    auto columnSize = segDocs.size();
    if (columnSize == 0) return;
    auto& fieldCol = columnsProto[field];

    if (!fieldType.multiValued()) {
      auto& vecsProto = *fieldCol.mutable_col_vec()->mutable_v();
      vecsProto.Reserve(columnSize);
      for (auto i = 0u; i < columnSize; i++) vecsProto.Add();
      std::span<solux::proto::Vector*> target{vecsProto.mutable_data(), columnSize};

      int32_t start = 0;
      for (auto runlen : segRunLength) {
        auto idxSpan = sortedIdx.subspan(start, runlen);
        task_group_run(tg, [this, idxSpan, &req, field, &segDocs, target]() {
          loadVectorColForSegmentSingle(req, field, idxSpan, segDocs, target);
        });
        start += runlen;
      }
    } else {
      auto& arrVecsProto = *fieldCol.mutable_multi_vec()->mutable_v();
      arrVecsProto.Reserve(columnSize);
      for (auto i = 0u; i < columnSize; i++) arrVecsProto.Add();
      std::span<solux::proto::ArrVector*> target{arrVecsProto.mutable_data(), columnSize};

      int32_t start = 0;
      for (auto runlen : segRunLength) {
        auto idxSpan = sortedIdx.subspan(start, runlen);
        task_group_run(tg, [this, idxSpan, &req, field, &segDocs, target]() {
          loadVectorColForSegmentMulti(req, field, idxSpan, segDocs, target);
        });
        start += runlen;
      }
    }
  }

  // Copy raw column bytes into a Vector.f32's repeated float buffer.  memcpy
  // sidesteps alignment of the source bytes (column storage is byte-packed
  // and not guaranteed 4-byte aligned).
  static void fillVectorF32(solux::proto::Vector& vec, std::string_view bytes) {
    assert(bytes.size() % sizeof(float) == 0);
    int32_t nFloats = (int32_t)(bytes.size() / sizeof(float));
    auto& fs = *vec.mutable_f32()->mutable_v();
    fs.Resize(nFloats, 0.0f);
    std::memcpy(fs.mutable_data(), bytes.data(), bytes.size());
  }

  void loadVectorColForSegmentSingle(SearchRequest& req, std::string_view field,
                                     std::span<uint8_t> idxSpan,
                                     std::ranges::input_range auto& segDocs,
                                     std::span<solux::proto::Vector*> target)
  {
    auto sortedSegDocs = idxSpan | std::views::transform([&segDocs](auto idx) { return segDocs[idx]; });
    auto segNum = sortedSegDocs[0].segment();
    auto sortedDocs = sortedSegDocs | std::views::transform([](const auto& sd) { return sd.docId(); });

    auto& postingsReader = req.reader->segments()[segNum].postingsReader();
    auto poolGuard = MemPool::threadLocalPoolGuard();
    FieldReader fieldReader(poolGuard.pool(), postingsReader);
    if (!fieldReader.seek(field)) return;
    SegFieldInfo segFieldInfo;
    fieldReader.readFieldInfo(segFieldInfo);

    auto valHandler = [&](size_t idx, int32_t doc, std::string_view bytes) {
      assert(segDocs[idxSpan[idx]].docId() == doc);
      unused(doc);
      fillVectorF32(*target[idxSpan[idx]], bytes);
    };
    StrColReader::getValues(poolGuard.pool(), postingsReader, segFieldInfo, sortedDocs, valHandler);
  }

  void loadVectorColForSegmentMulti(SearchRequest& req, std::string_view field,
                                    std::span<uint8_t> idxSpan,
                                    std::ranges::input_range auto& segDocs,
                                    std::span<solux::proto::ArrVector*> target)
  {
    auto sortedSegDocs = idxSpan | std::views::transform([&segDocs](auto idx) { return segDocs[idx]; });
    auto segNum = sortedSegDocs[0].segment();
    auto sortedDocs = sortedSegDocs | std::views::transform([](const auto& sd) { return sd.docId(); });

    auto& postingsReader = req.reader->segments()[segNum].postingsReader();
    auto poolGuard = MemPool::threadLocalPoolGuard();
    FieldReader fieldReader(poolGuard.pool(), postingsReader);
    if (!fieldReader.seek(field)) return;
    SegFieldInfo segFieldInfo;
    fieldReader.readFieldInfo(segFieldInfo);

    auto valHandler = [&](size_t idx, int32_t doc, std::string_view bytes, int64_t valIdx, int64_t numVals) {
      assert(segDocs[idxSpan[idx]].docId() == doc);
      unused(doc);
      auto* outer = target[idxSpan[idx]]->mutable_v();
      if (valIdx == 0) outer->Reserve(numVals);
      fillVectorF32(*outer->Add(), bytes);
    };
    StrColReader::getMultiValues(poolGuard.pool(), postingsReader, segFieldInfo, sortedDocs, valHandler);
  }

  // Copy stored values for one doc into a request's output slot.
  static void storeValuesInReq(const StoredReq& r, size_t slot, std::span<const std::string_view> values) {
    if (values.empty()) return;
    if (!r.multi) {
      *r.starget[slot] = std::string(values[0]);
    } else {
      solux::proto::ArrStr& target = *r.mtarget[slot];
      target.mutable_v()->Reserve((int)values.size());
      for (auto v : values) {
        auto* strProto = target.mutable_v()->Add();
        *strProto = std::string(v);
      }
    }
  }

  // Dispatch stored-fields retrieval for all fields that share one
  // resource.  Each segment run launches a single task that opens one
  // StoredFieldsReader and decodes every field in the group from the
  // shared decompressed chunk.
  //
  // Per-segment the group is partitioned into (a) fields that exist in the
  // segment's stored-fields resource — served from the chunk — and
  // (b) fields that don't — served from their column as a fallback, or
  // left empty if the field also lacks a column.  This handles both
  // pre-STORED segments (no SFR at all) and partial-stored segments (SFR
  // present but missing a specific field added later).
  void loadStoredFields(SearchRequest& req, std::string_view resourceName,
                        std::vector<StoredReq> reqsOwned,
                        std::ranges::input_range auto& segDocs, std::span<uint8_t> sortedIdx,
                        const std::span<uint8_t> segRunLength, oneapi::tbb::task_group* tg)
  {
    // Move into a shared_ptr so every segment task can safely reference the
    // same vector even after this function returns.
    auto reqs = std::make_shared<std::vector<StoredReq>>(std::move(reqsOwned));

    int32_t start = 0;
    for (auto runlen : segRunLength) {
      auto idxSpan = sortedIdx.subspan(start, runlen);
      task_group_run(tg, [this, idxSpan, &req, resourceName, reqs, &segDocs]() {
        unused(this);
        auto segNum = segDocs[idxSpan[0]].segment();
        auto& postingsReader = req.reader->segments()[segNum].postingsReader();
        auto sfr = StoredFieldsReader::open(postingsReader, resourceName);

        // Partition reqs: sfrReqs serve from SFR, colReqs fall back to
        // column.  The fallback is attempted for any STRING/ID field type
        // (not just fields whose current schema sets hasColumn): older
        // segments predating a schema change may still have the ord/value
        // column on disk, and loadStrColForSegment no-ops gracefully if the
        // field is actually absent.  TEXT fields have no retrievable column
        // — they stay empty when missing from the segment's SFR.
        std::vector<StoredReq*> sfrReqs;
        std::vector<StoredReq*> colReqs;
        sfrReqs.reserve(reqs->size());
        for (auto& r : *reqs) {
          if (sfr && sfr->hasField(r.fieldName)) {
            sfrReqs.push_back(&r);
            continue;
          }
          auto t = r.fieldType->type();
          if (t == FieldType::Type::STRING || t == FieldType::Type::ID) {
            colReqs.push_back(&r);
          }
        }

        for (auto* r : colReqs) {
          loadStrColForSegment(req, r->fieldName, *r->fieldType, idxSpan, segDocs, r->starget, r->mtarget);
        }

        if (sfrReqs.empty()) return;

        if (sfrReqs.size() == 1) {
          // Single-field fast path: readFieldById skips other stored
          // fields in each doc without materializing their value views.
          // Resolve fid once per segment rather than on every doc call.
          auto* r = sfrReqs[0];
          int32_t fid = sfr->fieldId(r->fieldName);
          assert(fid >= 0);  // partitioning above guarantees presence
          for (int32_t i = 0; i < (int32_t)idxSpan.size(); i++) {
            int32_t doc = segDocs[idxSpan[i]].docId();
            sfr->readFieldById(doc, fid, [&](std::span<const std::string_view> values) {
              storeValuesInReq(*r, idxSpan[i], values);
            });
          }
        } else {
          // Multi-field path: one readDoc pass per doc, matching each
          // entry against the sfr-backed request list.  Typical size is
          // small so a linear scan is fine.
          for (int32_t i = 0; i < (int32_t)idxSpan.size(); i++) {
            int32_t doc = segDocs[idxSpan[i]].docId();
            sfr->readDoc(doc, [&](std::string_view name, std::span<const std::string_view> values) {
              for (auto* r : sfrReqs) {
                if (r->fieldName == name) {
                  storeValuesInReq(*r, idxSpan[i], values);
                  break;
                }
              }
            });
          }
        }
      });
      start += runlen;
    }
  }

  std::unique_ptr<DocSet> getDocSet(Query::Weight& weight, int32_t segnum) {
    auto poolGuard = MemPool::threadLocalPoolGuard();
    auto* scorer = weight.createScorer(poolGuard.pool(), req.reader->segments()[segnum]);
    DocSetBuilder builder(req.reader->segments()[segnum].maxDoc());
    if (scorer != nullptr) {
      for (;;) {
        auto doc = scorer->next();
        if (doc == PostingsReader::END) {
          break;
        }
        builder.add(doc);
      }
    }
    return builder.build();
  }

};


}
