#include "Codec.h"

namespace solux {

/**
 * This is a modified version of BasicSortedBitPacker from SIMDCompressionLib
 * that removes all the dynamic memory allocation.
 */
class SoluxBitPacker {
private:
  constexpr static uint32_t SIZE = 128;
  uint32_t sizes[32];  // TODO: these can be made smaller
  uint32_t data[32][128];  // 16K

  SoluxBitPacker(const SoluxBitPacker &) = delete;
  SoluxBitPacker &operator=(const SoluxBitPacker &) = delete;
public:
  uint32_t buffer[32];  // TODO: why public, and why needed?

  static std::string name() { return "soluxBP"; }

  SoluxBitPacker() {
    clear();
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

// TODO: we could save more memory by reusing the same thread-local instance of the bitpacker.
using SoluxPForType = SIMDCompressionLib::SIMDFastPFor<4, SIMDCompressionLib::NoDelta, SoluxBitPacker>;
static thread_local std::unique_ptr<SoluxPForType> soluxPfor = nullptr;

void SoluxPFOR::encodeBlock(uint32_t* in, uint32_t inSz, char* out, uint32_t& outSz) {
  auto codec = soluxPfor.get();
  if (codec == nullptr) {
    codec = (soluxPfor = std::make_unique<SoluxPForType>()).get();
  }
  size_t compressedSize = outSz / sizeof(uint32_t); // this gets changed to the actual size... simdcomp lib uses size in units of words.

  // encode single block only
  assert(inSz == 128);
  auto prev = _mm_set1_epi32(0);
  codec->__encodeArray(in, 128, (uint32_t*)out, compressedSize, prev);
  outSz = compressedSize * sizeof(uint32_t);  // convert to bytes
}

uint32_t SoluxPFOR::decodeBlock(const char* in, uint32_t inSz, uint32_t* out, uint32_t& outSz) {
  auto codec = soluxPfor.get();
  if (codec == nullptr) {
    codec = (soluxPfor = std::make_unique<SoluxPForType>()).get();
  }

  // decode single block only
  assert(outSz == 128);
  size_t wordsRead;  // amount of compressed data read (in units of words instead of bytes)
  auto prev = _mm_set1_epi32(0);
  codec->__decodeArray((uint32_t*)in, wordsRead, out, 128, prev);
  return wordsRead * sizeof(uint32_t);
}

}  // namespace solux