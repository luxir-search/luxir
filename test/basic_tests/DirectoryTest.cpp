
#include <gtest/gtest.h>
#include <iostream>

#include "solux/store/Directory.h"
#include "test/SoluxTest.h"

using namespace solux;

class DirectoryTest : public solux::SoluxTest {
protected:
  void addFile(Directory& dir, const std::string& name, const std::string& data, std::vector<std::string>& resultListing) {
    resultListing.resize(0);

    std::unique_ptr<File> f = dir.createFile(name);
    OutputStream os;
    char arr[6];
    // sometimes start off with a user supplied buffer for the output stream
    if (rng.rbool()) {
      os = OutputStream(arr, arr+sizeof(arr));
    }

    os.setFile(f.get());
    os.write(data.data(), data.size());
    // os.write('X');  // make sure test fails with this
    ASSERT_EQ(data.size(), os.size());
    os.close();
    ASSERT_EQ(data.size(), f->size());
    dir.finishFile(*f);

    // check if dir contents are in sorted order
    dir.listFiles(resultListing);
    for (uint32_t i=1; i<resultListing.size(); i++) {
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

    auto input2 = dir.openFile(name);  // open again... to test out shared_ptr + resource management
    ASSERT_EQ(input->read().size(), input2->read().size());
    ASSERT_EQ(input->read().data(), input2->read().data());
  }

  void doDir(Directory& dir) {
    std::vector<std::string> lst;

    dir.listFiles(lst);
    ASSERT_EQ(lst.size(), 0);

    addFile(dir, "f5", "12345", lst);
    ASSERT_EQ("f5", lst[0]);

    // add file at end
    addFile(dir, "f5a", "123456", lst);
    ASSERT_EQ("f5a", lst[1]);

    // add file at start
    std::string aaa_data = "123456789abcdefghijklmnopqrstuvwxyz";
    addFile(dir, "aaa", aaa_data, lst);
    ASSERT_EQ("aaa", lst[0]);

    // add file in middle
    addFile(dir, "bbb", "qwertyuiop", lst);
    ASSERT_EQ("bbb", lst[1]);

    // remove a non-existing file
    bool found = dir.deleteFile("doesntexist");
    ASSERT_EQ(false, found);

    // remove a non-existing file that would appear at the end of the directory listing
    found = dir.deleteFile("zzzzzzzzzzzzzzzzzzzzzzzzzzzz");
    ASSERT_EQ(false, found);

    // remove existing file from the middle
    found = dir.deleteFile("bbb");
    ASSERT_EQ(true, found);

    // make sure it's gone
    auto input = dir.openFile("bbb");
    ASSERT_TRUE(input.get() == nullptr);

    lst.resize(0);
    dir.listFiles(lst);
    ASSERT_EQ(lst.size(), 3);
    ASSERT_EQ("f5", lst[1]);

    // test delete of open file
    input = dir.openFile("aaa");
    found = dir.deleteFile("aaa");
    ASSERT_EQ(true, found);
    auto data = input->read();
    ASSERT_EQ(data.size(), aaa_data.size());
    ASSERT_EQ(0, memcmp(data.data(), aaa_data.data(), data.size()));
  }
};


TEST_F(DirectoryTest, ramdir) {
  RAMDir dir;
  doDir(dir);
}
