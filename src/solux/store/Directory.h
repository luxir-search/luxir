#pragma once

#include <vector>
#include <string_view>
#include "OutputStream.h"

namespace solux {

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

// TODO: currently not thread safe
class RAMDir : public Directory {
public:
  using OutputFileType = RAMFile;
  using InputFileType = RAMInputFile;
  using InputReferenceType = std::shared_ptr<InputFileType>;

private:
  using entry_type = std::pair<std::string, InputReferenceType>;
  using iterator_type = std::vector<entry_type>::iterator;

  // RAMDir uses a sorted vector to minimize the additional space requirements when there are tons of directories.
  // Insertion will be fast since files are also generally produced in sorted order (although removing old ones will be slightly slower)
  // We should still benchmark (time and space) vs a good ordered_map implementation in the future though.
  std::vector<entry_type> files;

  // returns <found,iterator> pair... iterator is the element if found==true or the insertion point if found==false.
  std::pair<bool, iterator_type> find(const std::string_view &name) {
    auto iter = std::lower_bound(files.begin(), files.end(), name,
                                 [&](const entry_type &x, const std::string_view &key) { return x.first < key; }
    );
    return {!(iter == files.end() || iter->first != name), iter};
  }

public:


  void listFiles(std::vector<std::string> &target) override {
    target.reserve(files.size());
    for (const auto&[name, ifile] : files) {
      target.push_back(name);
    }
  }

  std::shared_ptr<InputFile> openFile(const std::string_view &name) override {
    auto[found, iter] = find(name);
    if (found) {
      return iter->second;
    } else {
      return {};
    }
  }

  bool deleteFile(const std::string_view &name) override {
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

    // See if new file name is greater than all others produced (this is common by design)
    if (files.empty() || files.back().first <
                         file.name()) {  // TODO: what is clang-tidy's problem with this line??? It suggests replacing "<" with nullptr !??
      files.emplace_back(file.name(), std::move(inputFile));
    } else {
      auto[found, iter] = find(file.name());
      if (found) {
        // overwrite
        *iter = {file.name(), std::move(inputFile)};
      } else {
        // insert
        files.insert(iter, {file.name(), std::move(inputFile)});
      }
    }
  }

};


// TODO: make filenames naturally sortable... prefix with the number of digits following.
// Extensions won't mess up this scheme since even if the "." is included in the sort, ord(".") < ord("0")
// Also prefix with an "s" for solux?   s10 s11 s12 .. s1z .. s210 s211 s2
// Multiple indexes in the same directory with a custom prefix (or prefix with the index name?)
// For the segments file... perhaps name it s_<version> since _ is between A and a (use caps for base36 and then the segments file will be last)
// Although I think we should have a cfs format that packs *everything* into a single file.
// Maybe name an all-in-one cfs the same as the segments file, so we only have to look at single files.
// TODO: what about different document types / tables?  Essentially mini indexes.
// What about multiple shards in a single directory?  Just have efficient dirs and have multiple dirs instead?
// How about setting overrides in a directory? Like number of partitions?  Seems like that should be at a higher / pluggable level.

// That would mess up the option to sync all files and then write a segments file, but many file systems don't need that.
// We should have a flexible enough container format to be able to include or pull out whatever files we want.

} // end namespace