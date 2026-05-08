#include "IndexReader.h"
#include "solux/reader/Postings.h"
#include "solux/reader/VectorAuxReader.h"

#include "protos/solux_types.pb.h"
#include <boost/unordered/unordered_flat_map.hpp>
#include <google/protobuf/arena.h>
#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/io/zero_copy_stream_impl_lite.h>

#include "OrdMapImpl.h"

namespace solux {

std::shared_ptr<LiveDocs> LiveDocs::create(Directory& dir, uint64_t segId, uint64_t liveGen, int32_t maxDoc, bool missingFileOK, bool expectSynced) {
  assert(liveGen > 0 && maxDoc > 0);
  std::string deleteFileName = Postings::getLiveDocsFileName(
    Postings::getSortableString(segId), liveGen);

  auto deleteFile = dir.openFile(deleteFileName, expectSynced);
  if (deleteFile == nullptr) {
    if (missingFileOK) {
      // Delete file not found - return nullptr instead of throwing
      IREADER_DEBUG("Delete file {} not found for segment {} (liveGen={})",
                deleteFileName, segId, liveGen);
      return nullptr;
    } else {
      throw std::filesystem::filesystem_error(
              std::format("Delete file {} not found for segment {} (liveGen={})",
                         deleteFileName, segId, liveGen),
              std::make_error_code(std::errc::no_such_file_or_directory));
    }
  }
  IREADER_DEBUG("Successfully opened delete file {} for segment {}", deleteFileName, segId);

  InputStream deleteStream = deleteFile->getInputStream();

  // Validate file format
  const char* data = deleteStream.ptr();
  size_t dataSize = deleteStream.size();

  // Define offsets based on field sizes
  auto headerSize =  Postings::SOLUX_HEADER.size() + sizeof(uint64_t) + sizeof(int32_t) + sizeof(int32_t);
  // Minimum size check
  if (dataSize < headerSize) {
    throw std::runtime_error(std::format("Delete file {} is too small: {} bytes", deleteFileName, dataSize));
  }

  // Check magic number
  if (std::memcmp(data, Postings::SOLUX_HEADER.data(), Postings::SOLUX_HEADER.size()) != 0) {
    throw std::runtime_error(std::format("Delete file {} has invalid magic number", deleteFileName));
  }

  deleteStream.skip(Postings::SOLUX_HEADER.size());
  uint64_t format = (uint64_t)deleteStream.readLong();
  int32_t fileMaxDoc = deleteStream.readInt();
  int32_t numLiveDocs = deleteStream.readInt();

  if (format != 1) {
    throw std::runtime_error(std::format("Delete file {} has unsupported format: {}", deleteFileName, format));
  }

  if (fileMaxDoc != maxDoc) {
    throw std::runtime_error(std::format("Delete file {} maxDoc mismatch: {} (expected {})",
              deleteFileName, fileMaxDoc, maxDoc));
  }

  if (numLiveDocs < 0 || numLiveDocs > maxDoc) {
    throw std::runtime_error(std::format("Delete file {} has invalid numLiveDocs: {} (maxDoc={})",
              deleteFileName, numLiveDocs, maxDoc));
  }

  // Validate file size
  int64_t expectedSize = deleteStream.offset() + screaming::FixedBitSet::sizeInWords(maxDoc) * sizeof(uint64_t);
  if (deleteStream.size() != expectedSize) {
    throw std::runtime_error(std::format("Delete file {} size mismatch: {} bytes (expected {})",
              deleteFileName, dataSize, expectedSize));
  }

  // FixedBitSet with live docs - memory map directly!
  uint64_t* liveBitsMemory = (uint64_t*)deleteStream.ptr();

  IREADER_DEBUG("Loaded {} deleted documents from {} for segment {}, {} live docs",
            maxDoc - numLiveDocs, deleteFileName, segId, numLiveDocs);

  return std::make_shared<LiveDocs>(liveBitsMemory, maxDoc, std::move(deleteFile), numLiveDocs);
}


IndexReader::IndexReader(Directory& dir, IndexReader* previousReader) {
  // because old segments could be merged away before we have a chance to read them, we need
  // to check if there is a new index info file and retry the open if so.
  // This could be optimized by saving the segments we did read properly in case they are still in the index.
  // But we should really have a postings getter abstraction that can provide already opened readers and livedocs
  uint64_t lastCommitTime = 0;
  bool retry = false;
  
  google::protobuf::Arena arena;

  // Index previous reader's aux readers by name for cheap reuse when the new
  // commit carried the entry forward (same gen + built_core_gen).  FAISS
  // deserialization is the expensive part; reuse keeps reopens cheap.
  boost::unordered_flat_map<std::string, std::shared_ptr<AuxReader>> prevAuxByName;
  if (previousReader) {
    for (const auto& r : previousReader->auxReadersList) {
      prevAuxByName.emplace(std::string(r->getName()), r);
    }
  }

  do {
    if (retry) {
      IREADER_DEBUG("Retrying IndexReader open");
      segs.clear();
      auxReadersList.clear();
      totalMaxDoc = 0;
      livedocs = 0;
      retry = false;
      // Clear arena to prevent unbounded growth
      arena.Reset();
    }
    
    auto* indexInfo = google::protobuf::Arena::Create<solux::proto::IndexInfo>(&arena);
    
    // Don't use expectSynced here - IndexReader can race with a concurrent
    // commit that has finished s.olux but not yet synced it.
    std::shared_ptr<InputFile> inputFile = dir.openFile(Postings::INDEX_INFO_FILE);
    if (inputFile == nullptr) {
      IREADER_DEBUG("No {} file, Empty IndexReader", Postings::INDEX_INFO_FILE);
    }
    else {
      IREADER_DEBUG("Opening IndexReader");
      InputStream segmentsIs = inputFile->getInputStream();

      google::protobuf::io::ArrayInputStream arrayStream(segmentsIs.ptr(), (int)segmentsIs.left());
      google::protobuf::io::CodedInputStream codedStream(&arrayStream);

      if (!indexInfo->ParseFromCodedStream(&codedStream)) {
        throw std::runtime_error("Failed to parse IndexInfo protobuf");
      }
      assert(codedStream.ConsumedEntireMessage());

      commitTimeUs = indexInfo->commit_time();
      this->coreGeneration = indexInfo->core_gen();
      bool missingFileOK = true; // Allow missing files on first attempt
      IREADER_DEBUG("\tOpening IndexReader, commitTime={} nSegs={} gen={}", indexInfo->commit_time(),
                    indexInfo->segments_size(), indexInfo->index_gen());
      if (commitTimeUs == lastCommitTime) {
        // No new commit, continue with missingFileOK=false so we get proper exceptions
        IREADER_DEBUG("Retry index open did not get new IndexInfo file, will try with missingFileOK=false.");
        missingFileOK = false; // On retry, we expect files to be present
      }
      lastCommitTime = commitTimeUs;

      segs.reserve(indexInfo->segments_size());
      for (int i = 0; i < indexInfo->segments_size(); i++) {
        const auto& segment = indexInfo->segments(i);
        uint64_t segId = segment.seg_id();
        uint64_t liveGen = segment.live_gen();
        int32_t nDocs = segment.max_doc();
        unused(nDocs);

        // TODO: instead of creating a new PostingsReader, we could check if the previousReader has it already opened.
        // Also, to be more flexible, we should probably pass in a provider interface that can provide PostingsReaders and LiveDocs
        // from other sources (like cached in IndexWriter, or from previous IndexReader).
        auto postingsReader = PostingsReader::create(dir, segId, missingFileOK, true);
        if (!postingsReader) {
          // Failed to create PostingsReader (segment files not found) - trigger retry
          retry = true;
          break; // Break out of the segments loop to retry
        }

        std::shared_ptr<LiveDocs> liveDocs;
        if (liveGen > 0) {
          liveDocs = LiveDocs::create(dir, segId, liveGen, nDocs, missingFileOK, true);
          if (!liveDocs) {
            // Failed to create LiveDocs (delete file not found) - trigger retry
            retry = true;
            break; // Break out of the segments loop to retry
          }
        }
        // If liveGen == 0, liveDocs remains nullptr (no deletes)

        Segment::SegmentInfo segmentInfo;
        segmentInfo.seg_id = segId;
        segmentInfo.live_gen = liveGen;
        segmentInfo.min_version = segment.min_version();
        segmentInfo.max_version = segment.max_version();
        segmentInfo.max_doc = nDocs;
        segmentInfo.commit_time = segment.commit_time();
        segmentInfo.live_docs = segment.live_docs();

        segs.emplace_back(std::move(postingsReader), std::move(liveDocs), segmentInfo, totalMaxDoc, i);
        totalMaxDoc += segs.back().postingsReader().maxDoc();
        livedocs += segs.back().numLive();
        assert(nDocs == segs.back().postingsReader().maxDoc());
      }

      // Aux indexes: open inside the same retry loop so a missing aux file
      // converts to a re-parse of IndexInfo (writer deletes orphaned aux files
      // only after publishing the new IndexInfo, so the retry will see a
      // referenceable list).  Unknown-kind entries are skipped silently.
      if (!retry) {
        for (int i = 0; i < indexInfo->aux_indexes_size(); i++) {
          const auto& info = indexInfo->aux_indexes(i);

          // Reuse from the previous reader when name + gen + built_core_gen
          // all match - the writer carry-forward logic guarantees the files
          // are byte-identical in that case.
          auto prevIt = prevAuxByName.find(std::string(info.name()));
          if (prevIt != prevAuxByName.end()
              && prevIt->second->getGen() == info.gen()
              && prevIt->second->getBuiltCoreGen() == info.built_core_gen()) {
            IREADER_DEBUG("Reusing aux reader '{}' (gen={}) from previous IndexReader",
                          info.name(), info.gen());
            auxReadersList.push_back(prevIt->second);
            continue;
          }

          // Dispatch by kind.  Unknown kinds are silently skipped so older
          // binaries can read indexes that contain newer aux kinds.
          std::shared_ptr<AuxReader> aux;
          if (info.kind() == VectorAuxReader::KIND) {
            aux = VectorAuxReader::open(dir, info, missingFileOK);
          } else {
            IREADER_DEBUG("Skipping unknown aux kind '{}' for '{}'", info.kind(), info.name());
            continue;
          }

          if (!aux) {
            IREADER_DEBUG("Aux file missing for '{}' - triggering retry", info.name());
            retry = true;
            break;
          }
          auxReadersList.push_back(std::move(aux));
        }
      }
    }
  }
  while (retry);

  // Check if we can share ordMaps from the previous reader
  if (previousReader && previousReader->coreGen() == this->coreGeneration) {
    IREADER_DEBUG("Sharing ordMaps from previous IndexReader (coreGen={})", this->coreGeneration);
    this->ordMaps = previousReader->ordMaps;
  } else {
    IREADER_DEBUG("Creating new ordMaps cache (coreGen={})", this->coreGeneration);
    this->ordMaps = std::make_shared<SharedLazyMap<std::string, OrdMap>>();
  }

  IREADER_DEBUG("IndexReader opened with {} segments and {} docs, commitTime={}", segs.size(), totalMaxDoc, commitTimeUs);
}



}