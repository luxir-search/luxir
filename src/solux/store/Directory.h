#pragma once

#include <vector>
#include <string_view>
#include "OutputStream.h"


class Directory {
public:
  virtual std::unique_ptr<File> createFile(std::string name) = 0;
  virtual void finishFile(File& file) = 0;  // make the file readable to others

};

class RAMDir : public Directory {
public:
  using OutputFileType = RAMFile;
  using InputFileType = RAMInputFile;
  using entry_type = std::pair<std::string, InputFileType>;

  std::vector<entry_type> files;

  std::unique_ptr<File> createFile(std::string name) override {
    return std::make_unique<RAMFile>(std::move(name));
  }

  void finishFile(File& file) override {
    auto& ramFile = dynamic_cast<RAMFile&>(file);
    auto sz = ramFile.size();
    // don't use make_unique as it uselessly zeroes memory first.
    std::unique_ptr<char[]> singleBuffer(new char[sz]);
    ramFile.copyTo(singleBuffer.get());
    // TODO: maintain sorted order
    files.emplace_back(file.name(), RAMInputFile(std::move(singleBuffer), sz));
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
