#pragma once

#include <vector>
#include <string_view>
#include "OutputStream.h"

namespace solux {

// Implementations of Directory are thread safe.
class Directory {
public:
  // TODO: add a prefix option?
  // appends a list of names to the referenced vector
  virtual void listFiles(std::vector<std::string> &target) = 0;

  virtual std::shared_ptr<InputFile> openFile(const std::string_view &name) = 0;

  virtual std::unique_ptr<File> createFile(const std::string_view &name) = 0;

  // Returns true if file was found and deleted, false if not found.
  virtual bool deleteFile(const std::string_view &name) = 0;

  // Make the file readable to others through the Directory.  Putting this on the Directory class
  // gives more flexibility in implementation without having every File have to point back to it's
  // owning Directory.
  virtual void finishFile(File &file) = 0;
};


class RAMDir : public Directory {
public:
  using OutputFileType = RAMFile;
  using InputFileType = RAMInputFile;
  using InputReferenceType = std::shared_ptr<InputFileType>;

private:
  using entry_type = std::pair<std::string, InputReferenceType>;
  using iterator_type = std::vector<entry_type>::iterator;
  std::mutex mutex;

  // RAMDir uses a sorted vector to minimize the additional space requirements when there are tons of directories.
  // Insertion will be fast since files are also generally produced in sorted order (although removing old ones will be slightly slower)
  // We should still benchmark (time and space) vs a good ordered_map implementation in the future though.
  // TODO: bulk operations (open/delete) could be done more efficiently (by prefix)
  // TODO: C++23 has a std::flat_set now (as does boost), so we could use that.
  std::vector<entry_type> files;

  // returns <found,iterator> pair... iterator is the element if found==true or the insertion point if found==false.
  std::pair<bool, iterator_type> find(const std::string_view &name) {
    auto iter = std::lower_bound(files.begin(), files.end(), name,
                                 [&](const entry_type &x, const std::string_view &key) { return x.first < key; }
    );
    return {!(iter == files.end() || iter->first != name), iter};
  }

public:
  RAMDir() = default;

  // Only used by tests to clear (e.g. dir = RAMDir())
  void operator=(RAMDir&& other) {
    std::lock_guard<std::mutex> lock(mutex);
    files = std::move(other.files);
  }

  void listFiles(std::vector<std::string> &target) override {
    std::lock_guard<std::mutex> lock(mutex);
    target.reserve(files.size());
    for (const auto&[name, ifile] : files) {
      target.push_back(name);
    }
  }

  std::shared_ptr<InputFile> openFile(const std::string_view &name) override {
    std::lock_guard<std::mutex> lock(mutex);
    auto[found, iter] = find(name);
    if (found) {
      return iter->second;
    } else {
      return {};
    }
  }

  bool deleteFile(const std::string_view &name) override {
    std::lock_guard<std::mutex> lock(mutex);
    auto[found, iter] = find(name);
    if (found) {
      files.erase(iter);
      return true;
    } else {
      return false;
    }
  }

  std::unique_ptr<File> createFile(const std::string_view &name) override {
    return std::make_unique<OutputFileType>(name);
  }

  void finishFile(File &file) override {
    auto &ramFile = dynamic_cast<OutputFileType &>(file);
    auto sz = ramFile.size();
    // don't use make_unique as it uselessly zeroes memory first.
    std::unique_ptr<char[]> singleBuffer(new char[sz]);
    ramFile.copyTo(singleBuffer.get());
    auto inputFile = std::make_shared<RAMInputFile>(std::move(singleBuffer), sz);

    {
      std::lock_guard<std::mutex> lock(mutex);
      // See if new file name is greater than all others produced (this is common by design)
      if (files.empty() || files.back().first < file.name()) {
        files.emplace_back(file.name(), std::move(inputFile));
      } else {
        auto [found, iter] = find(file.name());
        if (found) {
          // overwrite
          *iter = {file.name(), std::move(inputFile)};
        } else {
          // insert
          files.insert(iter, {file.name(), std::move(inputFile)});
        }
      }
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

};


} // end namespace