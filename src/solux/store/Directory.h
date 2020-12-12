#pragma once

#include <vector>
#include <string_view>
#include "OutputStream.h"


class Directory {
public:
  // TODO: add a prefix option?
  // appends a list of names to the referenced vector
  virtual void listFiles(std::vector<std::string>& target) = 0;

  virtual std::shared_ptr<InputFile> openFile(const std::string& name) = 0;

  virtual std::unique_ptr<File> createFile(const std::string& name) = 0;

  // Make the file readable to others through the Directory.  Putting this on the Directory class
  // gives more flexibility in implementation without having every File have to point back to it's
  // owning Directory.
  virtual void finishFile(File& file) = 0;
};

// TODO: currently not thread safe
class RAMDir : public Directory {
public:
  using OutputFileType = RAMFile;
  using InputFileType = RAMInputFile;
  using InputReferenceType = std::shared_ptr<InputFileType>;
  using entry_type = std::pair<std::string, InputReferenceType>;

  // RAMDir uses a sorted vector to minimize the additional space requirements when there are tons of directories.
  // Insertion will be fast since files are also generally produced in sorted order (although removing old ones will be slightly slower)
  // We should still benchmark (time and space) vs a good ordered_map implementation in the future though.
  std::vector<entry_type> files;

  void listFiles(std::vector<std::string> &target) override {
    target.reserve(files.size());
    for (const auto&[name, ifile] : files) {
      target.push_back(name);
    }
  }

  std::shared_ptr<InputFile> openFile(const std::string& name) override {
    auto ptr = std::lower_bound(files.begin(), files.end(), name,
                                            [&](const entry_type & x, const std::string& key) { return x.first < key; }
    );
    if (ptr == files.end() || ptr->first !=  name) {
      return {};
    } else {
      return ptr->second;
    }
  }

  std::unique_ptr<File> createFile(const std::string& name) override {
    return std::make_unique<RAMFile>(name);
  }

  void finishFile(File& file) override {
    auto& ramFile = dynamic_cast<RAMFile&>(file);
    auto sz = ramFile.size();
    // don't use make_unique as it uselessly zeroes memory first.
    std::unique_ptr<char[]> singleBuffer(new char[sz]);
    ramFile.copyTo(singleBuffer.get());
    auto inputFile = std::make_shared<RAMInputFile>(std::move(singleBuffer), sz);

    // See if new file name is greater than all others produced (this is common by design)
    if (files.empty() || files.back().first < file.name()) {  // TODO: what is clang-tidy's problem with this line??? It suggests replacing "<" with nullptr !??
      files.emplace_back(file.name(), std::move(inputFile));
    } else {
      // file does not belong at end, so let's find insertion point to maintain sorted order.
      auto insertion_point = std::lower_bound(files.begin(), files.end(), file.name(),
                       [&](const entry_type & x, const std::string& key) { return x.first < key; }
                       );
      // Insertion_point won't be at end since we already detected that common case.
      // Check for an overwrite though (overwrite should be a flag somewhere...)
      if (insertion_point->first == file.name()) {
        // overwrite (or throw/return error if we're not supposed to overwrite or it's unexpected)
        *insertion_point = {file.name(), std::move(inputFile)};
        // insertion_point->second = RAMInputFile(std::move(singleBuffer), sz);  // alternative that just changes one element of the pair?
      } else {
        // insert
        files.insert(insertion_point, {file.name(), std::move(inputFile)});
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
