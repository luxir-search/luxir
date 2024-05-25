#pragma once

#include <cstdint>
#include <cassert>
#include "solux/util/solux_util.h"

// TODO: eventually hide this in the cpp
#include "simdcomp/include/codecfactory.h"

// these are defined in frameofreference.cpp in SIMDCompressionAndIntersection
// need to remove "static" from some so we can access them.
__m128i* simdpackFOR_length(uint32_t initvalue, const uint32_t *in,
                              int length, __m128i *out,
                              const uint32_t bit);
void simdpackFOR(uint32_t initvalue, const uint32_t *in, __m128i *out,
                   const uint32_t bit);
const __m128i* simdunpackFOR_length(uint32_t initvalue,
                                      const __m128i *in, int length,
                                      uint32_t *out, const uint32_t bit);
void simdunpackFOR(uint32_t initvalue, const __m128i *in, uint32_t *out,
                     const uint32_t bit);
void simdmaxmin_length(const uint32_t *in, uint32_t length,
                         uint32_t *getmin, uint32_t *getmax);


namespace solux {



class U32Codec {
public:
  virtual ~U32Codec() = default;

  // input size is in ints, output size is size in bytes of the buffer.
  // outSz is updated to reflect how much data was written.
  virtual void encodeBlock(uint32_t* in, uint32_t inSz, char* out, uint32_t &outSz) = 0;

  // inSz is in bytes, outSz is number of ints.
  // outSz is updated to reflect how many ints were written.
  // returns the number of bytes read from the input.
  virtual uint32_t decodeBlock(const char* in, uint32_t inSz, uint32_t* out, uint32_t &outSz) = 0;

  virtual uint32_t select(const char* compressed, uint32_t blockSize, uint32_t index) {
    unused(compressed);
    unused(blockSize);
    unused(index);
    throw new std::runtime_error("select not implemented for this codec");
  }
};

// U64 codec can handle both 32 and 64 bit integers.
class U64Codec {
public:
  virtual ~U64Codec() = default;

  // input size is in ints, output size is size in bytes of the buffer.
  // outSz is updated to reflect how much data was written.
  virtual void encodeBlock(uint32_t* in, uint32_t inSz, char* out, uint32_t &outSz) = 0;

  // inSz is in bytes, outSz is number of ints.
  // outSz is updated to reflect how many ints were written.
  // returns the number of bytes read from the input.
  virtual uint32_t decodeBlock(const char* in, uint32_t inSz, uint32_t* out, uint32_t &outSz) = 0;

  // TODO: 64 bit variants
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


/// NOTE: due to the way SIMD is implemented, reads can happen past the end of the block.
/// As long as reads are valid for up to 16-31 bytes past the end of the block, we are fine.
/// Add 16 and then round up to 16 bytes.
class SoluxSIMDFor : public U32Codec {
  // this codec seems thread safe.  The class has no state.
  SIMDCompressionLib::SIMDFrameOfReference codec;
public:
  ~SoluxSIMDFor() override = default;


  void encodeWithMeta(uint32_t* in, uint32_t inSz, char* target, uint32_t &outSz, uint32_t minval, uint8_t bits) {
    uint32_t* out = (uint32_t*)target;
    uint32_t k = 0;
    for (; k + 128 <= inSz; k += 128, in += 128) {
      simdpackFOR(minval, in, (__m128i *)out, bits);
      out += bits * 4;
    }
    if (inSz != k)
      out = (uint32_t *)simdpackFOR_length(minval, in, inSz - k, (__m128i *)out, bits);
    in += inSz - k;
    outSz = (char*)out - target;
  }


  uint32_t decodeWithMeta(const char* encoded, uint32_t inSz, uint32_t* out, uint32_t &outSz, uint32_t minval, uint8_t bits) {
    const uint32_t* in = (const uint32_t*)encoded;
    for (uint32_t k = 0; k < outSz / 128; ++k) {
      simdunpackFOR(minval, (const __m128i *)(in + 4 * bits * k), out + 128 * k, bits);
    }
    out = out + outSz / 128 * 128;
    in = in + outSz / 128 * 4 * bits;
    if ((outSz % 128) != 0) {
      in = (const uint32_t*) simdunpackFOR_length(
              minval, (__m128i*) in, outSz - outSz / 128 * 128, out, bits);
    }
    return (char*)in - encoded;
  }


  void encodeBlock(uint32_t* in, uint32_t inSz, char* target, uint32_t &outSz) override final {
    uint32_t* out = (uint32_t*)target;
    // This should be equivalent to a call to simd_compress_length.  We just want
    // to lower the number of code paths now that we have encodeWithMeta (and exercise it).
    if (inSz == 0) {
      outSz=0;
      return;
    }
    uint32_t m, M;
    simdmaxmin_length(in, inSz, &m, &M);
    int b = std::bit_width(static_cast<uint32_t>(M - m));
    out[0] = m;
    ++out;
    out[0] = M;
    ++out;

    encodeWithMeta(in, inSz, (char*)out, outSz, m, b);
    outSz += 2 * sizeof(uint32_t);

    /* // previous code before encodeWithMeta
    // call the internal version that skips writing the number of values
    // The first two words written are the min and max values.  If we need to calculate these ourselves, they could
    // be passed in?  We will really need gcd coding to handle things like dates and doubles.
    auto end = codec.simd_compress_length(in, inSz, (uint32_t*)out);
    assert((char*)end - (char*)out <= outSz);  // if not enough space passed in, we overran buffer.
    outSz = (char*)end - (char*)out;
     */
  }

  // NOTE: the return type is not always correct.  There seems to be an issue with the underlying codec.simd_uncompress_length.
  uint32_t decodeBlock(const char* compressed, uint32_t inSz, uint32_t* out, uint32_t &outSz) override final {
    const uint32_t* in = (const uint32_t*)compressed;
    if (outSz == 0) {
      outSz = 0;
      return 0;
    }
    uint32_t m = in[0];
    ++in;
    uint32_t M = in[0];
    ++in;
    int b = std::bit_width(static_cast<uint32_t>(M - m));

    auto readSize = decodeWithMeta((const char*)in, inSz - 2 * sizeof(uint32_t), out, outSz, m, b);
    return readSize + 2 * sizeof(uint32_t);
    /*
    auto end = codec.simd_uncompress_length((uint32_t*)in, out, outSz);
    // don't update outSize, we depend on it already being correct.
    return (char*)end - in;
    */
  }

  static uint32_t staticselect(const char* compressed, uint32_t blockSize, uint32_t index) {
    SoluxSIMDFor c;
    return c.select(compressed, blockSize, index);
  }

  // blockSize is the number of values in this specific block, not necessarily our large block size of 16K
  uint32_t select(const char* compressed, uint32_t blockSize, uint32_t index) {
    // This is adapted from SIMDCompressionLib::SIMDFrameOfReference::select
    uint32_t* in = (uint32_t*)compressed;
    // uint32_t length = *in;
    // in++;
    uint32_t m = *in;
    ++in;
    uint32_t M = *in;
    ++in;
    uint32_t bit = std::bit_width(M - m);
    if (bit == 32) {
      return in[index];
    } else if (bit == 0) {
      return m;  // all values equal, nothing encoded.  This was missing from original, leading to OOB read.
    }
    in += index / 128 * 4 * bit;
    const uint32_t slot = index % 128;
    const uint32_t lane = slot % 4;               /* we have 4 interleaved lanes */
    const uint32_t bitsinlane = (slot / 4) * bit; /* how many bits in lane */
    const uint32_t firstwordinlane = bitsinlane / 32;
    const uint32_t secondwordinlane = (bitsinlane + bit - 1) / 32;
    const uint32_t firstpart =
            in[4 * firstwordinlane + lane] >> (bitsinlane % 32);
    const uint32_t mask = (1 << bit) - 1;
    if (firstwordinlane == secondwordinlane) {
      /* easy common case*/
      return m + (firstpart & mask);
    } else {
      /* harder case where we need to combine two words */
      const uint32_t secondpart = in[4 * firstwordinlane + 4 + lane];
      const uint32_t usablebitsinfirstword = 32 - (bitsinlane % 32);
      return m + ((firstpart | (secondpart << usablebitsinfirstword)) & mask);
    }
  }
};

class SoluxFor : public U32Codec {
  // this codec seems thread safe.  The class has no state.
  SIMDCompressionLib::ForCODEC codec;
public:
  ~SoluxFor() override = default;

  void encodeBlock(uint32_t* in, uint32_t inSz, char* out, uint32_t &outSz) override {
    size_t compressedSize = outSz / sizeof(uint32_t); // this gets changed to the actual size... simdcomp lib uses size in units of words.
    codec.encodeArray(in, inSz, (uint32_t*)out, compressedSize);
    outSz = compressedSize * sizeof(uint32_t);  // convert to bytes
  }

  uint32_t decodeBlock(const char* in, uint32_t inSz, uint32_t* out, uint32_t &outSz) override {
    uint64_t recoveredSz = outSz;
    auto endPtr = codec.decodeArray( (uint32_t*)in, inSz / sizeof(uint32_t), out, recoveredSz);
    outSz = recoveredSz;
    auto bytesRead = (char*)endPtr - (char*)in;
    // TODO: FIXME: this is currently triggering assert(bytesRead <= inSz);
    return bytesRead;
  }

  // blockSize is the number of values in this specific block, not necessarily our large block size of 16K
  uint32_t select(const char* compressed, uint32_t blockSize, uint32_t index) override final {
    return codec.select((uint32_t*)compressed,  index);
  }
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

  void encodeBlock(uint32_t* in, uint32_t inSz, char* out, uint32_t& outSz) override {
    size_t compressedSize = outSz /
                            sizeof(uint32_t); // this gets changed to the actual size... simdcomp lib uses size in units of words.
    getCodec().encodeArray(in, inSz, (uint32_t*) out, compressedSize);
    outSz = compressedSize * sizeof(uint32_t);  // convert to bytes
  }

  uint32_t decodeBlock(const char* in, uint32_t inSz, uint32_t* out, uint32_t& outSz) override {
    uint64_t recoveredSz = outSz;
    auto endPtr = getCodec().decodeArray((uint32_t*) in, inSz / sizeof(uint32_t), out, recoveredSz);
    outSz = recoveredSz;
    auto bytesRead = (char*) endPtr - (char*) in;
    // TODO: FIXME: this is triggering with IntegerCODECTypeWrapper::ForCODEC assert(bytesRead <= inSz);
    return bytesRead;
  }


  uint32_t select(const char* compressed, uint32_t blockSize, uint32_t index) override {
    // check if Type is SIMDCompressionLib::ForCODEC
    if constexpr (std::is_same<Type, SIMDCompressionLib::ForCODEC>::value) {
      return getCodec().select((uint32_t*) compressed, index);
    } else if constexpr(std::is_same<Type, SIMDCompressionLib::SIMDFrameOfReference>::value) {
      return getCodec().select((uint32_t*) compressed, index);
    }
      throw new std::runtime_error("select not implemented for this codec");
  }


};

template<typename T>
thread_local std::unique_ptr<T> IntegerCODECTypeWrapper<T>::codec = nullptr;

} // end namespace