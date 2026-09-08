// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <gtest/gtest.h>


class Book {
  std::string str;
  void readFile();
  void parse();

public:
  std::vector<int> bookOffsets;
  std::vector<int> chapterOffsets;
  std::vector<int> paraOffsets;
  std::vector<int16_t> paraSizes;
  int sumParaSizes;  // size of all paragraphs together (i.e. smaller than the whole text of the book)

  Book();

  std::string_view text() {
    return str;
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
