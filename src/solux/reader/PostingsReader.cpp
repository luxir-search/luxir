#include "PostingsReader.h"

namespace solux {

Postings::DocsCodec Postings::docCodec;
Postings::PositionsCodec Postings::posCodec;
Postings::TFreqCodec& Postings::tfreqCodec = Postings::posCodec;
Postings::NumericCodec Postings::numericCodec;



// Initialize from files, returns false if missing files and missingFileOK=true
bool PostingsReader::initializeFromFiles(Directory& dir, uint64_t segId, bool missingFileOK) {
  std::string segStr = Postings::getSortableString(segId);
  auto segInfoFile = Postings::getIndexFileName(segStr, 0);
  files.emplace_back(dir.openFile(segInfoFile));

  if (files.back().get() == nullptr) {
    if (missingFileOK) {
      return false;
    }
    throw std::filesystem::filesystem_error(
            std::format("Can't find/open first segment file '{}'", segInfoFile),
            std::make_error_code(std::errc::no_such_file_or_directory));
  }

  inputStreams.emplace_back(files[0]->getInputStream());
  InputStream firstIS = inputStreams[0];

  // seek to the end of firstIs and read the size of the segmentInfo
  // See PostingsWriter.writeSegmentInfo
  firstIS.seek(firstIS.size() - sizeof(int32_t));
  auto segInfoSize = firstIS.readInt();
  segInfoOffset = firstIS.size() - sizeof(int32_t) - segInfoSize;
  firstIS.seek(segInfoOffset);
  maxdoc = firstIS.readVint();
  int nFiles = firstIS.readVint();

  files.reserve(nFiles);
  inputStreams.reserve(nFiles);

  for (int i=1; i<nFiles; i++) {
    files.emplace_back(dir.openFile(Postings::getIndexFileName(segStr, i)));
    if (files.back().get() == nullptr) {
      if (missingFileOK) {
        return false;
      }
      throw std::filesystem::filesystem_error(
              std::format("Can't find/open segment file '{}'", Postings::getIndexFileName(segStr, i)),
              std::make_error_code(std::errc::no_such_file_or_directory));
    }
    inputStreams.emplace_back(files.back()->getInputStream());
  }
  return true;
}

std::shared_ptr<PostingsReader> PostingsReader::create(Directory& dir, uint64_t segId, bool missingFileOK) {
  std::string segStr = Postings::getSortableString(segId);
  auto segInfoFile = Postings::getIndexFileName(segStr, 0);
  auto firstFile = dir.openFile(segInfoFile);

  if (firstFile == nullptr) {
    if (missingFileOK) {
      return nullptr;
    }
    throw std::filesystem::filesystem_error(
            std::format("Can't find/open first segment file '{}'", segInfoFile),
            std::make_error_code(std::errc::no_such_file_or_directory));
  }

  // Try to create the PostingsReader - use private constructor
  auto reader = std::shared_ptr<PostingsReader>(new PostingsReader());
  if (!reader->initializeFromFiles(dir, segId, missingFileOK)) {
    return nullptr;  // Missing files and missingFileOK=true
  }
  return reader;
}

PostingsReader::PostingsReader(Directory& dir, uint64_t segId) {
  auto reader = create(dir, segId, false);
  // This should never fail (should throw exception on failure) since missingFileOK=false, but just in case
  if (!reader) {
    throw std::filesystem::filesystem_error(
            "Failed to create PostingsReader",
            std::make_error_code(std::errc::no_such_file_or_directory));
  }
  *this = std::move(*reader);
}

} // namespace solux