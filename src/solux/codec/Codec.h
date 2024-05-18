#pragma once

#include <cstdint>
#include <cassert>
#include "solux/util/solux_util.h"

// TODO: eventually hide this in the cpp
#include "simdcomp/include/codecfactory.h"

namespace solux {



class U32Codec {
public:
  virtual ~U32Codec() = default;

  // input size is in ints, output size is size in bytes of the buffer.
  // outSz is updated to reflect how much data was written.
  virtual void encodeBlock(uint32_t* in, uint32_t inSz, char* out, uint32_t &outSz) = 0;

  // inSz is in bytes, outSz is number of ints.
  // outSz is updated to reflect how many ints were written.
  // returns the number of bytes read from the input (TODO: update inSz for symmetry?)
  virtual uint32_t decodeBlock(const char* in, uint32_t inSz, uint32_t* out, uint32_t &outSz) = 0;
};



/// No compression, just remembers the length and uses memcpy to copy the array
class SimpleCodec : public U32Codec {
public:
  ~SimpleCodec() override = default;

  void encodeBlock(uint32_t* in, uint32_t inSz, char* out, uint32_t &outSz) override {
    *reinterpret_cast<uint32_t*>(out) = inSz;
    memcpy(out+sizeof(uint32_t), in, inSz * sizeof(uint32_t));
    outSz = (inSz+1)*sizeof(uint32_t);
  }

  // Hmmm, some codecs may be able to derive the size of the encoded data, and some may not!
  uint32_t decodeBlock(const char* in, uint32_t inSz, uint32_t* out, uint32_t &outSz) override {
    unused(inSz);
    outSz = *reinterpret_cast<const uint32_t*>(in);
    assert((outSz+1)*sizeof(uint32_t) <= inSz);
    memcpy(out, in+sizeof(uint32_t), outSz*sizeof(uint32_t));
    return (outSz+1)*sizeof(uint32_t);
  }
};

class SoluxPFOR : public U32Codec {
public:
  // Micro-benchmarks show decoding 256 takes about 35% longer to 50% longer (with stack bitpacker) than 128.
  // So that's still a savings for dense iteration, but a drawback for very sparse.
  const static uint32_t BLOCK_SIZE = 128;

  ~SoluxPFOR() override = default;

  void encodeBlock(uint32_t* in, uint32_t inSz, char* out, uint32_t &outSz) override;

  uint32_t decodeBlock(const char* in, uint32_t inSz, uint32_t* out, uint32_t &outSz) override;
};

// delta version of the codec
class SoluxPFORd : public U32Codec {
public:
  ~SoluxPFORd() override = default;

  void encodeBlock(uint32_t* in, uint32_t inSz, char* out, uint32_t &outSz) override;

  uint32_t decodeBlock(const char* in, uint32_t inSz, uint32_t* out, uint32_t &outSz) override;
};

// Wraps types of SIMDCompressionLib::IntegerCODEC to make them thread-safe (via thread-local)
// and to translate the interface to U32Codec
template <class Type>  // Type should be subclass of SIMDCompressionLib::IntegerCODEC
class IntegerCODECTypeWrapper : public U32Codec {
  thread_local static std::unique_ptr<Type> codec;
public:

  IntegerCODECTypeWrapper() {
    // codec = std::make_unique<Type>();
  }

  Type& getCodec() {
    if (!codec) {
      codec = std::make_unique<Type>();
    }
    return *codec;
  }

  void encodeBlock(uint32_t* in, uint32_t inSz, char* out, uint32_t &outSz) override {
    size_t compressedSize = outSz / sizeof(uint32_t); // this gets changed to the actual size... simdcomp lib uses size in units of words.
    getCodec().encodeArray(in, inSz, (uint32_t*)out, compressedSize);
    outSz = compressedSize * sizeof(uint32_t);  // convert to bytes
  }

  uint32_t decodeBlock(const char* in, uint32_t inSz, uint32_t* out, uint32_t &outSz) override {
    uint64_t recoveredSz = outSz;
    auto endPtr = getCodec().decodeArray( (uint32_t*)in, inSz / sizeof(uint32_t), out, recoveredSz);
    outSz = recoveredSz;
    auto bytesRead = (char*)endPtr - (char*)in;
    assert(bytesRead <= inSz);
    return bytesRead;
  }
};

template<typename T>
thread_local std::unique_ptr<T> IntegerCODECTypeWrapper<T>::codec = nullptr;

} // end namespace