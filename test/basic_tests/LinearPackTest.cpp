#include "luxir/codec/LinearPack.h"

#include <random>
#include <vector>

#include <gtest/gtest.h>

using namespace luxir;

namespace {

std::vector<uint64_t> randomValues(uint32_t count, uint8_t bits) {
  std::mt19937_64 random(1234567 + bits);
  uint64_t mask = LinearPack::mask64(bits);
  std::vector<uint64_t> values(count);
  for (uint64_t& value : values) value = random() & mask;
  return values;
}

std::vector<char> packToBuffer(const std::vector<uint64_t>& values, uint8_t bits) {
  std::vector<char> encoded(LinearPack::byteSize(values.size(), bits), (char)0x7f);
  LinearPack::Writer writer(encoded.data(), bits);
  for (uint64_t value : values) writer.append(value);
  EXPECT_EQ(encoded.size(), writer.finish());
  return encoded;
}

} // namespace

TEST(LinearPackTest, roundTripEveryUint32Width) {
  constexpr uint32_t count = 333;
  for (uint8_t bits = 0; bits <= 32; bits++) {
    std::vector<uint64_t> values = randomValues(count, bits);
    std::vector<char> encoded = packToBuffer(values, bits);
    uint32_t mask = LinearPack::mask32(bits);
    EXPECT_EQ(values.front(), LinearPack::select32(encoded.data(), 0, bits, mask));
    EXPECT_EQ(values.back(), LinearPack::select32(encoded.data(), count - 1, bits, mask));
    for (uint32_t i = 0; i < count; i++) {
      ASSERT_EQ(values[i], LinearPack::select32(encoded.data(), i, bits, mask))
          << "bits=" << (int)bits << " index=" << i;
    }
  }
}

TEST(LinearPackTest, unpackMatchesSelectEveryUint32Width) {
  for (uint8_t bits = 0; bits <= 32; bits++) {
    for (uint32_t totalCount : {128u, 205u}) {
      std::vector<uint64_t> values = randomValues(totalCount, bits);
      std::vector<char> encoded = packToBuffer(values, bits);
      uint32_t decoded[128];
      uint32_t mask = LinearPack::mask32(bits);
      uint32_t start = totalCount == 128 ? 0 : 128;
      uint32_t frameCount = totalCount - start;
      LinearPack::unpack128(
          encoded.data(), start, frameCount, bits, mask, decoded);
      for (uint32_t i = 0; i < frameCount; i++) {
        ASSERT_EQ(
            LinearPack::select32(encoded.data(), start + i, bits, mask),
            decoded[i])
            << "bits=" << (int)bits << " index=" << start + i;
      }
    }
  }
}

TEST(LinearPackTest, contiguousLittleEndianLayout) {
  std::vector<uint64_t> values = {1, 2, 3, 4};
  std::vector<char> encoded = packToBuffer(values, 3);
  ASSERT_EQ(2 + LinearPack::TAIL_PAD, encoded.size());
  EXPECT_EQ(0xd1, (uint8_t)encoded[0]);
  EXPECT_EQ(0x08, (uint8_t)encoded[1]);
  for (size_t i = 2; i < encoded.size(); i++) EXPECT_EQ(0, encoded[i]);
}

TEST(LinearPackTest, select64EveryWidth) {
  constexpr uint32_t count = 139;
  for (uint8_t bits = 0; bits <= 57; bits++) {
    std::vector<uint64_t> values = randomValues(count, bits);
    std::vector<char> encoded = packToBuffer(values, bits);
    uint64_t mask = LinearPack::mask64(bits);
    for (uint32_t i : {0u, 1u, 63u, 127u, count - 1}) {
      ASSERT_EQ(values[i], LinearPack::select64(encoded.data(), i, bits, mask))
          << "bits=" << (int)bits << " index=" << i;
    }
    uint64_t decoded[128];
    LinearPack::unpack128(encoded.data(), 0, 128, bits, mask, decoded);
    for (uint32_t i = 0; i < 128; i++) EXPECT_EQ(values[i], decoded[i]);
    LinearPack::unpack128(encoded.data(), 128, count - 128, bits, mask, decoded);
    for (uint32_t i = 128; i < count; i++) EXPECT_EQ(values[i], decoded[i - 128]);
  }
}

TEST(LinearPackTest, streamSizeAndTailPad) {
  for (uint8_t bits = 0; bits <= 32; bits++) {
    std::vector<uint64_t> values = randomValues(131, bits);
    RAMFile file("linear-pack");
    OutputStream out(&file);
    LinearPack::Writer writer(out, bits);
    for (uint64_t value : values) writer.append(value);
    uint64_t written = writer.finish();
    EXPECT_EQ(LinearPack::byteSize(values.size(), bits), written);
    EXPECT_EQ(written, out.size());
    out.close();
    EXPECT_EQ(written, file.size());

    std::vector<char> encoded(file.size());
    file.copyTo(encoded.data());
    for (uint64_t i = LinearPack::packedByteSize(values.size(), bits);
         i < encoded.size(); i++) {
      EXPECT_EQ(0, encoded[i]);
    }
  }
}
