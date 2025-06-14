#pragma once

#include <vector>
#include <string_view>
#include <gtl/btree.hpp>
#include "solux/util/log.h"
#include "OutputStream.h"

// redefine DEBUG to TRACE level which shouldn't currently be logged!
#define DIR_DEBUG LOG_TRACE
// #define DIR_DEBUG LOG_DEBUG

namespace solux {

// Implementations of Directory are thread safe.
class Directory {
public:
  // TODO: add a prefix option?
  // appends a list of names to the referenced vector
  virtual void listFiles(std::vector<std::string> &target) = 0;

  virtual std::shared_ptr<InputFile> openFile(const std::string_view name) = 0;

  virtual std::unique_ptr<File> createFile(const std::string_view name) = 0;

  // Returns true if file was found and deleted, false if not found.
  virtual bool deleteFile(const std::string_view name) = 0;

  virtual void deletePrefix(const std::string_view prefix) {
    std::vector<std::string> files;
    listFiles(files);
    for (const auto &file : files) {
      if (file.starts_with(prefix)) {
        deleteFile(file);
      }
    }
  }

  // Make the file readable to others through the Directory.  Putting this on the Directory class
  // gives more flexibility in implementation without having every File have to point back to it's
  // owning Directory.
  virtual void finishFile(File &file) = 0;

  // remove all files from the directory
  virtual void clear() = 0;

  virtual ~Directory() = default;
};


class RAMDir : public Directory {
public:
  using OutputFileType = RAMFile;
  using InputFileType = RAMInputFile;
  using InputReferenceType = std::shared_ptr<InputFileType>;

private:
  gtl::btree_map<std::string, InputReferenceType> files;
  std::mutex mutex;

public:
  RAMDir() = default;

  // Only used by tests to clear (e.g. dir = RAMDir())
  void operator=(RAMDir&& other) {
    std::lock_guard<std::mutex> lock(mutex);
    files = std::move(other.files);
  }

  void listFiles(std::vector<std::string>& target) override {
    std::lock_guard<std::mutex> lock(mutex);
    target.reserve(files.size());
    for (const auto&[name, ifile] : files) {
      target.push_back(name);
    }
  }

  std::shared_ptr<InputFile> openFile(const std::string_view name) override {
    std::lock_guard<std::mutex> lock(mutex);

    auto find = files.find(name);
    if (find != files.end()) {
      // redefine DEBUG to TRACE level which shouldn't currently be logged!
      // #define INDEX_DEBUG LOG_TRACE
      DIR_DEBUG("DIR openFile: found file {} size={}", name, find->second->size());
      return find->second;
    } else {
      DIR_DEBUG("DIR openFile: file {} not found", name);
      return {};
    }
  }

  bool deleteFile(const std::string_view name) override {
    std::lock_guard<std::mutex> lock(mutex);

    bool success = files.erase(name);
    DIR_DEBUG("DIR deleteFile {} success={}", name, success);
    return success;
  }

  void deletePrefix(const std::string_view prefix) override {
    std::lock_guard<std::mutex> lock(mutex);

    // Would erase range be more efficient here?  The number of items
    // to be erased is relatively small (number of files in a segment)
    for (auto it = files.lower_bound(prefix); it != files.end(); ) {
      if (!it->first.starts_with(prefix)) {
        break;
      }
      it = files.erase(it);
    }
  }

  std::unique_ptr<File> createFile(const std::string_view name) override {
    DIR_DEBUG("DIR about to createFile {}", name);
    return std::make_unique<OutputFileType>(name);
  }

  void finishFile(File &file) override {
    DIR_DEBUG("DIR about to finishFile {} size={}", file.name(), file.size());

    auto &ramFile = dynamic_cast<OutputFileType &>(file);
    auto sz = ramFile.size();
    std::unique_ptr<char[]> singleBuffer = std::make_unique_for_overwrite<char[]>(sz);
    ramFile.copyTo(singleBuffer.get());
    auto inputFile = std::make_shared<RAMInputFile>(std::move(singleBuffer), sz);

    {
      std::lock_guard<std::mutex> lock(mutex);

      files.emplace_hint(files.end(), file.name(), inputFile);

      // overwrite existing entries.
      // TODO: we take pains to add files in order, so we should try a hint to add it at the end of the list.
      files[file.name()] = std::move(inputFile);
    }
  }

  int64_t totalFileSize() {
    std::lock_guard<std::mutex> lock(mutex);
    int64_t totalSize = 0;
    for (const auto&[name, ifile] : files) {
      totalSize += ifile->size();
    }
    return totalSize;
  };

  void clear() override {
    DIR_DEBUG("DIR about to clear all files");
    std::lock_guard<std::mutex> lock(mutex);
    files.clear();
  }

};


} // end namespace