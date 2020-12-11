
#include <gtest/gtest.h>
#include <iostream>

#include "solux/store/Directory.h"

void addFile(Directory& dir, std::string name, std::string data, std::vector<std::string>& resultListing) {
  resultListing.resize(0);

  std::unique_ptr<File> f = dir.createFile(name);
  OutputStream os;
  os.setFile(f.get());
  os.write(data.data(), data.size());
  ASSERT_EQ(data.size(), os.size());
  os.close();
  ASSERT_EQ(data.size(), f->size());
  dir.finishFile(*f);

  // TODO: retrieve the file and check the size

  // check if dir contents are in sorted order
  dir.listFiles(resultListing);
  for (int i=1; i<resultListing.size(); i++) {
    ASSERT_LT(resultListing[i-1], resultListing[i]);
  }
}

template <class DirType>
void doDir() {
  DirType dir;
  std::vector<std::string> lst;

  dir.listFiles(lst);
  ASSERT_EQ(lst.size(), 0);

  addFile(dir, "f5", "12345", lst);
  ASSERT_EQ("f5", lst[0]);

  // add file at end
  addFile(dir, "f5a", "123456", lst);
  ASSERT_EQ("f5a", lst[1]);

  // add file at start
  addFile(dir, "aaa", "1", lst);
  ASSERT_EQ("aaa", lst[0]);
}

TEST(Directory, test) {
  doDir<RAMDir>();
}
