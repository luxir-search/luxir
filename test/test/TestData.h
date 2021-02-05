#pragma once
#include <filesystem>
#include <iostream>
#include <fstream>
#include <gtest/gtest.h>
#include "solux/util/solux_util.h"
#include "solux/util/random.h"

namespace fs=std::filesystem;
using namespace solux;

class Book {
public:
  std::string str;


  Book() {
    readFile();
    parse();
  }

  std::string_view text() {
    return str;
  }

  void readFile() {
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

  void parse() {



  }

};

class TestData {
  std::unique_ptr<Book> book;
public:
  static std::unique_ptr<TestData> data;

  Book& getBook() {
    if (book.get() == nullptr) {
      book = std::make_unique<Book>();
    }
    return *book;
  }
};
