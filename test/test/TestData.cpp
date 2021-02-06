#include <filesystem>
#include <iostream>
#include <fstream>
#include "TestData.h"

namespace fs=std::filesystem;

std::unique_ptr<TestData> TestData::data = std::make_unique<TestData>();

Book::Book() {
  readFile();
  parse();
}

void Book::readFile() {
  auto fname = fs::temp_directory_path() / "solux" / "book.txt";
  std::ifstream file(fname, std::ios::binary);
  if (!file) {
    std::cout << "Couldn't read file " << fname << std::endl;
  }
  std::stringstream ss;
  ss << file.rdbuf();
  str = ss.str();
  std::cout << "LENGTH of " << fname << " is " << str.size() << std::endl;
}


void Book::parse() {
  auto sv = text();
  auto off = sv.find("\nBOOK ONE:") + 1;  // off is offset of the *start* of a non-blank line.
  auto end = sv.rfind("End of the Project Gutenberg EBook");

  size_t maxParaSize = 0;
  sumParaSizes = 0;

  for(;;) {
    size_t nextEOL;
    for(;;) {
      nextEOL = sv.find('\n', off);
      if (nextEOL - off > 2) { // allow for \r\n... but could be fooled by single char lines if they exist with \n only.
        // found a non-blank line
        break;
      }
      off = nextEOL + 1;
    }
    if (off >= end) break;
    auto currLine = sv.substr(off);
    // check for special strings
    if (currLine.starts_with("BOOK ")
        || currLine.starts_with("FIRST EPI")
        || currLine.starts_with("SECOND EPI")
            ) {
      bookOffsets.push_back(off);
      off = nextEOL + 1;
      continue;
    } else if (currLine.starts_with("CHAPTER ")) {
      chapterOffsets.push_back(off);
      off = nextEOL + 1;
      continue;
    }

    paraOffsets.push_back(off);
    // this is a normal paragraph... find the end of it.
    for(;;) {
      nextEOL = sv.find('\n', off);
      if (nextEOL - off <= 2) {
        // found a blank line (end of para)
        break;
      }
      off = nextEOL + 1;
    }
    auto paraSize = off - paraOffsets.back() + 1; // +1 to include the actual '\n' char in the para
    paraSizes.push_back(paraSize);
    maxParaSize = std::max(maxParaSize, paraSize);
    sumParaSizes += paraSize;
    off = nextEOL + 1;
  }

  paraOffsets.shrink_to_fit();
  paraSizes.shrink_to_fit();

  std::cout << "nChapters=" << chapterOffsets.size() << " nPara=" << paraOffsets.size()
            << " maxParaSize=" << maxParaSize << " sumParaSizes=" << sumParaSizes << std::endl;

  // sanity check
  // std::cout << "FIRST PARA:" << text().substr(paraOffsets[0], paraSizes[0]) << std::endl;
  // std::cout << "LAST PARA:" << text().substr(paraOffsets.back(), paraSizes.back()) << std::endl;
}

