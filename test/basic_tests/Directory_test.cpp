
#include <gtest/gtest.h>
#include <iostream>

#include "solux/store/Directory.h"

void addFile(Directory& dir, const std::string& name, const std::string& data, std::vector<std::string>& resultListing) {
  resultListing.resize(0);

  std::unique_ptr<File> f = dir.createFile(name);
  OutputStream os;
  os.setFile(f.get());
  os.write(data.data(), data.size());
  // os.write('X');  // make sure test fails with this
  ASSERT_EQ(data.size(), os.size());
  os.close();
  ASSERT_EQ(data.size(), f->size());
  dir.finishFile(*f);

  // check if dir contents are in sorted order
  dir.listFiles(resultListing);
  for (int i=1; i<resultListing.size(); i++) {
    ASSERT_LT(resultListing[i-1], resultListing[i]);
  }

  // test not finding a file
  auto missing = dir.openFile(name+"!!!!!!!!!!!!!");
  ASSERT_TRUE(missing.get() == nullptr);

  // test file just written
  auto input = dir.openFile(name);
  ASSERT_TRUE(input.get() != nullptr);
  ASSERT_EQ(data.size(), input->size());
  ASSERT_EQ(0, memcmp(data.data(), input->read().data(), data.size()));
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

  // add file in middle
  addFile(dir, "bbb", "22", lst);
  ASSERT_EQ("bbb", lst[1]);
}

TEST(Directory, test) {
  doDir<RAMDir>();
}
