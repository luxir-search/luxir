
#include "gtest/gtest.h"
#include <iostream>
#include "simdcomp/include/codecfactory.h"
#include "simdcomp/include/intersection.h"
#include "test/SoluxTest.h"

using namespace std;
using namespace SIMDCompressionLib;

class SIMDCompTest : public solux::SoluxTest {
};


// started from SIMDCompressionAndIntersection example.cpp
TEST_F(SIMDCompTest, testComp) {
  // We pick a CODEC
  // https://arxiv.org/pdf/1401.6399.pdf
  //
  // IntegerCODEC &codec = *CODECFactory::getFromName("s4-bp128-d4");  // fastest according to the paper?
  // IntegerCODEC &codec = *CODECFactory::getFromName("s4-bp128-d1");  // 11% slower, 7% smaller than d4?
  IntegerCODEC &mycodec = *CODECFactory::getFromName("s4-fastpfor-d1");  // best for space   // NOTE: block size==256 integers!  What is the page size for?
  solux::unused(mycodec);
  // NOTE: some codecs (like s4-fastpfor-d1) modify the input array to calculate deltas!
  // NOTE: SIMDCompressionAndIntersection puts 32 bit size at start!  Look at C version and see if it's easier to modify?


  map<std::string, shared_ptr<IntegerCODEC>> codecs;

  // TODO: These bit ratios for smaller blocks may include stuff we don't need.  Revisit the block size
  // after we have an optimized version.
//  codecs.emplace("pfor32", shared_ptr<IntegerCODEC>(new CompositeCodec<SIMDFastPFor<1, RegularDeltaSIMD>, leftovercodec>()));  // 25 bits
//  codecs.emplace("pfor64", shared_ptr<IntegerCODEC>(new CompositeCodec<SIMDFastPFor<2, RegularDeltaSIMD>, leftovercodec>()));  // 13 bits
codecs.emplace("pfor128", shared_ptr<IntegerCODEC>(new CompositeCodec<SIMDFastPFor<4, RegularDeltaSIMD>, leftovercodec>()));  // 8.13 bits
//  codecs.emplace("pfor256", shared_ptr<IntegerCODEC>(new CompositeCodec<SIMDFastPFor<8, RegularDeltaSIMD>, leftovercodec>()));  // 7.88 bits
//  codecs.emplace("streamvbyte", shared_ptr<IntegerCODEC>(new StreamVByteD1()));  // 10.2 bits/integer


  //sorted integers.
  // size_t N = 256;    // for N=256:  s4-bp128-d4 = 9.63 bits/int   s4-bp128-d1 = 8.63     s4-fastpfor-d1 = 7.88
  size_t N = 128;    // for N=256:  s4-bp128-d4 = 9.63 bits/int   s4-bp128-d1 = 8.63     s4-fastpfor-d1 = 7.88
  uint32_t val = 0;
  vector<uint32_t> mydata(N);
  for (uint32_t i = 0; i < N; ++i) {
    val += rng.rint(10) + 1;
    if (rng.rint(10) < 3) val += rng.rint(100);
    if (rng.rint(10) < 1) val += rng.rint(200);
    mydata[i] = val;
  }

  //
  // You need some "output" container. You are responsible
  // for allocating enough memory.
  //
  vector<uint32_t> compressed_output(N + 1024);
  // N+1024 should be plenty
  //


  for (auto& [key,codec] : codecs) {
    cout << "Trying codec " << key << endl;
    // NOTE: some codecs (like s4-fastpfor-d1) modify the input array to calculate deltas!
    // make a copy of the input data to we can compare to the output again after compress-decompress
    vector<uint32_t> mydata_copy;
    mydata_copy = mydata;

    size_t compressedsize = compressed_output.size();
    codec->encodeArray(mydata_copy.data(), mydata.size(), compressed_output.data(),
                      compressedsize);

    // display compression rate:
    cout << setprecision(3);
    cout << "You are using "
         << 32.0 * static_cast<double>(compressedsize) /
            static_cast<double>(mydata.size())
         << " bits per integer. " << endl;

    vector<uint32_t> mydataback;
    mydataback.resize(N);
    size_t recoveredsize = mydataback.size();
    //


    vector<uint32_t> unaligned_data(N + 1025);
    char* ptr = ((char*)unaligned_data.data()) + 1;
    memcpy(ptr, compressed_output.data(), compressedsize * sizeof(uint32_t) );

    // codec.decodeArray(compressed_output.data(), compressed_output.size(),mydataback.data(), recoveredsize);
    codec->decodeArray( (uint32_t*)ptr, compressedsize, mydataback.data(), recoveredsize);  // unaligned


    mydataback.resize(recoveredsize);

    //
    // That's it for compression!
    //
    if (mydataback != mydata) {
      throw runtime_error("bug!");
    }
  }


  //
  // Next we are going to test out intersection...
  //

  int matches = 0;
  val = 0;
  vector<uint32_t> mydata2(N);
  for (uint32_t i = 0; i < N; ++i) {
    if (rng.rint(10) < 1) {
      // 10% of the time, select a value from the other list so we will get some matches.
      for (auto other : mydata) { // n^2 alg here, but test lists are small
        if (other > val) {  // first value that fits the bill
          val = other;
          break;
        }
      }
    } else {
      val += rng.rint(10)+1;
    }

    mydata2[i] = val;

    for (auto other : mydata) { // calculate how many matches there should be
      if (other == val) matches++;
      if (other >= val) break;
    }
  }

  //
  // we are going to intersect mydata and mydata2 and write back
  // the result to mydata2
  //
  /***
  intersectionfunction inter =
          IntersectionFactory::getFromName("simd"); // using SIMD intersection

  size_t intersize = inter(mydata2.data(), mydata2.size(), mydata.data(),
                           mydata.size(), mydata2.data());
  ***/

  size_t intersize = SIMDintersection(mydata2.data(), mydata2.size(), mydata.data(),
                   mydata.size(), mydata2.data());


  mydata2.resize(intersize);
  mydata2.shrink_to_fit();
  ASSERT_EQ(matches, mydata2.size());
}
