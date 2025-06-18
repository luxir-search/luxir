#include "Codec.h"

namespace solux {


IndexCodec::DocsCodec IndexCodec::docCodec;
IndexCodec::PositionsCodec IndexCodec::posCodec;
IndexCodec::TFreqCodec& IndexCodec::tfreqCodec = IndexCodec::posCodec;
IndexCodec::NumericCodec IndexCodec::numericCodec;


uint32_t staticselect(SoluxSIMDFor& c, const char* compressed, uint32_t blockSize, uint32_t index) {
  return c.select(compressed, blockSize, index);
}


/**
 * This is a modified version of BasicSortedBitPacker from SIMDCompressionLib
 * that removes all the dynamic memory allocation.
 */
class SoluxBitPacker {
private:
  constexpr static uint32_t SIZE = SoluxPFOR::BLOCK_SIZE;
  // a byte works when size==128, bit it seemed a little slower (5%). short seemed fastest when on the stack.
  uint16_t sizes[32];
  uint32_t data[32][SIZE];  // 16K when BLOCK_SIZE is 128.

  SoluxBitPacker(const SoluxBitPacker &) = delete;
  SoluxBitPacker &operator=(const SoluxBitPacker &) = delete;
public:
  uint32_t buffer[32];  // TODO: why public, and why needed?

  static std::string name() { return "soluxBP"; }

  SoluxBitPacker() {
    // clear();  // not needed since reset is called when needed.
  }

  void reset() {
    clear();
  }

  void directAppend(uint32_t i, uint32_t val) { data[i][sizes[i]++] = val; }

  const uint32_t* get(int i) { return data[i]; }

  void ensureCapacity(int i, uint32_t datatoadd) {
    assert(i>= 0 && i <= 32);
    assert(sizes[i] + datatoadd <= SIZE);
    (void)i; (void)datatoadd;
  }

  void clear() {
    for (uint32_t i = 0; i < 32; ++i)
      sizes[i] = 0;
  }

  uint32_t *write(uint32_t *out) {
    uint32_t bitmap = 0;
    for (uint32_t k = 1; k < 32; ++k) {
      if (sizes[k] != 0)
        bitmap |= (1U << k);
    }
    *(out++) = bitmap;

    for (uint32_t k = 1; k < 32; ++k) {
      if (sizes[k] != 0) {
        *out = sizes[k];
        out++;
        uint32_t j = 0;
        for (; j + 128 <= sizes[k]; j += 128) {
          SIMDCompressionLib::usimdpackwithoutmask(&data[k][j], reinterpret_cast<__m128i *>(out),
                               k + 1);
          out += 4 * (k + 1);
        }
        // falling back on scalar
        for (; j < sizes[k]; j += 32) {
          SIMDCompressionLib::BitPackingHelpers::fastpackwithoutmask(&data[k][j], out, k + 1);
          out += k + 1;
        }
        out -= (j - sizes[k]) * (k + 1) / 32;
      }
    }
    return out;
  }
  const uint32_t *read(const uint32_t *in) {
    clear();
    const uint32_t bitmap = *(in++);

    for (uint32_t k = 1; k < 32; ++k) {
      if ((bitmap & (1U << k)) != 0) {
        sizes[k] = *in++;
        assert(sizes[k] <= SIZE);
        uint32_t j = 0;
        for (; j + 128 <= sizes[k]; j += 128) {

          SIMDCompressionLib::usimdunpack(reinterpret_cast<const __m128i *>(in), &data[k][j],
                      k + 1);
          in += 4 * (k + 1);
        }
        for (; j + 31 < sizes[k]; j += 32) {
          SIMDCompressionLib::BitPackingHelpers::fastunpack(in, &data[k][j], k + 1);
          in += k + 1;
        }
        uint32_t remaining = sizes[k] - j;
        memcpy(buffer, in, (remaining * (k + 1) + 31) / 32 * sizeof(uint32_t));
        uint32_t *bpointer = buffer;
        in += ((sizes[k] + 31) / 32 * 32 - j) / 32 * (k + 1);
        for (; j < sizes[k]; j += 32) {
          SIMDCompressionLib::BitPackingHelpers::fastunpack(bpointer, &data[k][j], k + 1);
          bpointer += k + 1;
        }
        in -= (j - sizes[k]) * (k + 1) / 32;
      }
    }
    return in;
  }

  // for debugging
  void sanityCheck() {
    for (uint32_t k = 0; k < 32; ++k) {
      if (sizes[k] > SIZE) {
        std::cerr << "overflow at " << k << std::endl;
        throw std::runtime_error("bug");
      }
      if (sizes[k] != 0) {
        std::cout << "k=" << k << std::endl;
        uint32_t mask = 0u;
        for (uint32_t j = 0; j < sizes[k]; ++j) {
          std::cout << data[k][j] << " ";
          mask |= data[k][j];
        }
        std::cout << std::endl;

        if (SIMDCompressionLib::gccbits(mask) > k + 1) {
          std::cerr << "At " << (k + 1) << " we have " << SIMDCompressionLib::gccbits(mask) << std::endl;
          throw std::runtime_error("bug");
        }
      }
    }
  }

};

// We could save more memory by reusing the same thread-local instance of the bitpacker.
// for now, we are sharing the SoluxPForType for both delta and non-delta coded.
// The size param is the block size, which is in units of PACK_SIZE (32).
using SoluxPForType = SIMDCompressionLib::SIMDFastPFor<SoluxPFOR::BLOCK_SIZE/32, SIMDCompressionLib::NoDelta, SoluxBitPacker>;
static thread_local std::unique_ptr<SoluxPForType> soluxPfor = nullptr;

inline static void encBlock(uint32_t* in, uint32_t inSz, char* out, uint32_t& outSz) {
  auto codec = soluxPfor.get();
  if (codec == nullptr) {
    codec = (soluxPfor = std::make_unique<SoluxPForType>()).get();
  }
  size_t compressedSize = outSz / sizeof(uint32_t); // this gets changed to the actual size... simdcomp lib uses size in units of words.

  // encode single block only
  assert(inSz == SoluxPFOR::BLOCK_SIZE);
  auto prev = _mm_set1_epi32(0);
  codec->__encodeArray(in, SoluxPFOR::BLOCK_SIZE, (uint32_t*)out, compressedSize, prev);
  outSz = compressedSize * sizeof(uint32_t);  // convert to bytes
}

void SoluxPFOR::encodeBlock(uint32_t* in, uint32_t inSz, char* out, uint32_t& outSz) {
  return encBlock(in, inSz, out, outSz);
}

void SoluxPFORd::encodeBlock(uint32_t* in, uint32_t inSz, char* out, uint32_t& outSz) {
  auto prev = _mm_set1_epi32(0);
  SIMDCompressionLib::SIMDDeltaProcessor<SIMDCompressionLib::RegularDeltaSIMD, SoluxPFOR::BLOCK_SIZE>::runDelta(prev, in);
  return encBlock(in, inSz, out, outSz);
}

#ifdef CURRENTLY_FOR_REFERENCE_ONLY
// modified version of SIMDFastPFor::__decodeArray that just puts our bitpacker on the stack.
// void __decodeArray(SoluxBitPacker& bpacker, uint32_t *in, size_t &length, uint32_t *out,   // could also pass in bpacker from thread-local instance
static void __decodeArray(uint32_t *in, size_t &length, uint32_t *out, const size_t nvalue) {
SoluxBitPacker bpacker;  // big object (16K) on the stack, but this is pretty much a leaf call with no chance to work-steal.
constexpr auto BlockSize = SoluxPFOR::BLOCK_SIZE;
// constexpr auto nvalue = BlockSize;
constexpr auto arraydispatch = true;  // doesn't seem to matter much.


  const uint32_t *const initin = in;
  const uint32_t *const headerin = in++;
  const uint32_t wheremeta = headerin[0];
  const uint32_t *inexcept = headerin + wheremeta;
  const uint32_t bytesize = *inexcept++;
  const uint8_t *bytep = reinterpret_cast<const uint8_t *>(inexcept);

  inexcept += (bytesize + sizeof(uint32_t) - 1) / sizeof(uint32_t);
  inexcept = bpacker.read(inexcept);
  length = inexcept - initin;
  const uint32_t *unpackpointers[32 + 1];
  for (uint32_t k = 1; k <= 32; ++k) {
    unpackpointers[k] = bpacker.get(k - 1);
  }
#ifdef USE_ALIGNED
  in = padTo128bits(in);
    assert(!needPaddingTo128Bits(out));
#endif
  for (uint32_t run = 0; run < nvalue / BlockSize; ++run, out += BlockSize) {
    const uint8_t b = *bytep++;
    const uint8_t cexcept = *bytep++;
    for (uint32_t k = 0; k < BlockSize; k += 128) {
      if (arraydispatch)  /// hmmm, is this switched? -YCS
        SIMDCompressionLib::simdunpack(reinterpret_cast<const __m128i *>(in), out + k, b);
      else
        SIMDCompressionLib::ArrayDispatch::SIMDunpack(reinterpret_cast<const __m128i *>(in),
                                  out + k, b);
      in += 4 * b;
    }
    if (cexcept > 0) {
      const uint8_t maxbits = *bytep++;
      if (maxbits - b == 1) {
        for (uint32_t k = 0; k < cexcept; ++k) {
          const uint8_t pos = *(bytep++);
          out[pos] |= static_cast<uint32_t>(1) << b;
        }
      } else {
        const uint32_t *vals = unpackpointers[maxbits - b];
        unpackpointers[maxbits - b] += cexcept;
        for (uint32_t k = 0; k < cexcept; ++k) {
          const uint8_t pos = *(bytep++);
          out[pos] |= vals[k] << b;
        }
      }
    }
  }

  assert(in == headerin + wheremeta);
}
#endif // current for reference only

// modified version of __decodeArray where nvalue == BlockSize.
// this allowed us to get rid of unpackpointers
static void __decodeBlock(uint32_t *in, size_t &length, uint32_t *out) {
  SoluxBitPacker bpacker;  // big object (16K) on the stack, but this is pretty much a leaf call with no chance to work-steal.
  constexpr auto BlockSize = SoluxPFOR::BLOCK_SIZE;
// constexpr auto nvalue = BlockSize;
  constexpr auto arraydispatch = true;  // doesn't seem to matter much.

  const uint32_t *const initin = in;
  const uint32_t *const headerin = in++;
  const uint32_t wheremeta = headerin[0];
  const uint32_t *inexcept = headerin + wheremeta;
  const uint32_t bytesize = *inexcept++;
  const uint8_t *bytep = reinterpret_cast<const uint8_t *>(inexcept);

  inexcept += (bytesize + sizeof(uint32_t) - 1) / sizeof(uint32_t);
  inexcept = bpacker.read(inexcept);
  length = inexcept - initin;
#ifdef USE_ALIGNED
  in = padTo128bits(in);
    assert(!needPaddingTo128Bits(out));
#endif
  // for (uint32_t run = 0; run < nvalue / BlockSize; ++run, out += BlockSize) {
    const uint8_t b = *bytep++;
    const uint8_t cexcept = *bytep++;
    for (uint32_t k = 0; k < BlockSize; k += 128) {
      if (arraydispatch)  /// hmmm, is this switched? -YCS
        SIMDCompressionLib::simdunpack(reinterpret_cast<const __m128i *>(in), out + k, b);
      else
        SIMDCompressionLib::ArrayDispatch::SIMDunpack(reinterpret_cast<const __m128i *>(in),
                                                      out + k, b);
      in += 4 * b;
    }
    if (cexcept > 0) {
      const uint8_t maxbits = *bytep++;
      if (maxbits - b == 1) {
        for (uint32_t k = 0; k < cexcept; ++k) {
          const uint8_t pos = *(bytep++);
          out[pos] |= static_cast<uint32_t>(1) << b;
        }
      } else {
        const uint32_t *vals = bpacker.get(maxbits - b - 1);
        // unpackpointers[maxbits - b] += cexcept;
        for (uint32_t k = 0; k < cexcept; ++k) {
          const uint8_t pos = *(bytep++);
          out[pos] |= vals[k] << b;
        }
      }
    }
  // }

  assert(in == headerin + wheremeta);
}

inline static uint32_t decBlock(const char* in, uint32_t inSz, uint32_t* out, uint32_t& outSz) {
  unused(inSz);
  /* Not needed when we are using our own stack based bit-packer */
/*
  auto codec = soluxPfor.get();
  if (codec == nullptr) {
    codec = (soluxPfor = std::make_unique<SoluxPForType>()).get();
  }
*/

  // decode single block only
  assert(outSz == SoluxPFOR::BLOCK_SIZE);
  size_t wordsRead;  // amount of compressed data read (in units of words instead of bytes)
  // auto prev = _mm_set1_epi32(0);
  // codec->__decodeArray((uint32_t*)in, wordsRead, out, SoluxPFOR::BLOCK_SIZE, prev);
  // __decodeArray(codec->bpacker, (uint32_t*)in, wordsRead, out, SoluxPFOR::BLOCK_SIZE, prev);
  // __decodeArray((uint32_t*)in, wordsRead, out, SoluxPFOR::BLOCK_SIZE);
  __decodeBlock((uint32_t*)in, wordsRead, out);
  // Compiler randomness warning: for some reason, passing SoluxPFOR::BLOCK_SIZE as the last param, instead of
  // putting constexpr auto nvalues=SoluxPFOR::BLOCK_SIZE in the implementation is faster by 11.6% (gcc-13.1)
  return wordsRead * sizeof(uint32_t);
}

uint32_t SoluxPFOR::decodeBlock(const char* in, uint32_t inSz, uint32_t* out, uint32_t& outSz) {
  return decBlock(in, inSz, out, outSz);
}

uint32_t SoluxPFORd::decodeBlock(const char* in, uint32_t inSz, uint32_t* out, uint32_t& outSz) {
  auto ret = decBlock(in, inSz, out, outSz);
  auto prev = _mm_set1_epi32(0);
  SIMDCompressionLib::SIMDDeltaProcessor<SIMDCompressionLib::RegularDeltaSIMD, SoluxPFOR::BLOCK_SIZE>::runPrefixSum(prev, out);
  return ret;
}

}  // namespace solux