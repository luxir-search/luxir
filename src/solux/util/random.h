#pragma once

#include <cstdint>
#include <bit>


namespace solux {


/* rng benchmarks before xorshift64 was deleted (gcc10.2 on linux64, ryzen3600):
 * With clang 11, SplitMix64 was faster than RomuTrio, but xorshift64 was
 * still slower than both.
   --------------------------------------------------------------
   Benchmark                    Time             CPU   Iterations
   --------------------------------------------------------------
   BM_mersenne_twister       2.44 ns         2.44 ns    295362944
   BM_RomuTrio              0.716 ns        0.716 ns    979621083
   BM_SplitMix64            0.983 ns        0.983 ns    720882466
   BM_xorshift64             1.51 ns         1.51 ns    461584809
*/


// Adapted from http://prng.di.unimi.it/splitmix64.c (ORIG LICENSE: CC0 / public domain)
// Although not the highest quality, it's simplicity is good for matching random sequences in different languages
// since it's easy to port.  xorshift64 was previously used for this purpose, but this is both faster
// and does not have issues with a 0 seed.
class SplitMix64 {
  uint64_t x;
public:
  explicit SplitMix64() {} // unseeded! call init() before using.
  explicit SplitMix64(uint64_t seed) : x(seed) {}

  void init(uint64_t seed) {
    x = seed;
  }

  uint64_t operator()() {
    uint64_t z = (x += 0x9e3779b97f4a7c15);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9;
    z = (z ^ (z >> 27)) * 0x94d049bb133111eb;
    return z ^ (z >> 31);
  }

  // Simple biased modulo division.  Designed to be easily reproducible in other languages like Java.
  int64_t rint(int64_t max) {
    // mask off sign bit to ensure it's positive
    return (operator()() & 0x7fffffffffffffff) % max;
  }
};

// Adapted from https://www.romu-random.org/romupaper.pdf  (ORIG LICENSE: Apache 2)
// This was chosen for it's speed, relative quality, while still being easy to implement in a portable way.
class RomuTrio {
  uint64_t xState, yState, zState;

public:
  /// Expert! Directly seed internal state without any mixing.  The seeds should
  /// be high quality, or the first number of generated values should be discarded.
  ///  In all cases, there should be at least one non-zero state.
  void init(uint64_t seed1, uint64_t seed2, uint64_t seed3) {
    xState = seed1;
    yState = seed2;
    zState = seed3;
  }

  /// Seed from a normal (potentially low quality) seed.  A separate PRNG is used to
  /// generate a high quality internal state from the provided seed.
  void init(uint64_t seed) {
    // xState will be used unchanged to return the first value, so we should either discard the first
    // value, or use another PRNG to initialize the seed.  If initialization speed is important, we
    // really probably only need to have a good seed for xState.
    SplitMix64 seeder(seed);
    init(seeder(), seeder(), seeder());
  }

  explicit RomuTrio(uint64_t seed = 0) {
    init(seed);
  }

  uint64_t operator()() {
    uint64_t xp = xState, yp = yState, zp = zState;
    xState = 15241094284759029579u * zp;
    yState = yp - xp;
    yState = std::rotl(yState, 12);
    zState = zp - yp;
    zState = std::rotl(zState, 44);
    return xp;
  }
};


// TODO: implement interfaces expected of std random engines so we can use these in conjunction with
// other standard lib random code (like distribution generation)
template<class Engine>
class SoluxRand {
  Engine engine;
public:
  SoluxRand() {
  }

  explicit SoluxRand(uint64_t seed) : engine(seed) {
  }

  void init(uint64_t seed) {
    engine.init(seed);
  }

  uint64_t operator()() {
    return engine();
  };

  uint64_t rlong() {
    return engine();
  }


  /*
   * rint() methods use simple modulo division.  This introduces a slight bias (esp for large limits)
   * but is probably the fastest way if the limit/max being passed in is a constant since
   * optimizers will normally replace that division with other bitwise operations.
   */
  uint64_t rint(uint64_t max) {
    return engine() % max;
  }
  uint32_t rint(uint32_t max) {
    return static_cast<uint32_t>(engine()) % max;
  }
  int64_t rint(int64_t max) {
    return (engine()&0x7fffffffffffffff) % max;
  }
  int32_t rint(int32_t max) {
    return (engine()&0x7fffffff) % max;
  }


  //
  // Instead of using "%", use Daniel Lemire's nearly-divisionless random integer generation
  // https://lemire.me/blog/2019/06/06/nearly-divisionless-random-integer-generation-on-various-systems/
  // This is good if bias is not acceptable, or if the limit is not a constant as viewed by the compiler.
  //
  uint64_t rint2(uint64_t s) {
    uint64_t x = engine();
    auto m = static_cast<__uint128_t>(x) * static_cast<__uint128_t>(s);
    auto l = static_cast<uint64_t>(m);
    if (l < s) {
      uint64_t t = -s % s;
      while (l < t) {
        x = engine() ;
        m = static_cast<__uint128_t>(x) * static_cast<__uint128_t>(s);
        l = static_cast<uint64_t>(m);
      }
    }
    return m >> 64;
  }

  uint32_t rint2(uint32_t s) {
    auto x = static_cast<uint32_t>(engine());
    auto m = static_cast<uint64_t>(x) * static_cast<uint64_t>(s);
    auto l = static_cast<uint32_t>(m);
    if (l < s) {
      uint32_t t = -s % s;
      while (l < t) {
        x = engine() ;
        m = static_cast<uint64_t>(x) * static_cast<uint64_t>(s);
        l = static_cast<uint32_t>(m);
      }
    }
    return m >> 32;
  }

  int64_t rint2(int64_t max) {
    return static_cast<int64_t>( rint(static_cast<uint64_t>(max)) );
  }

  int32_t rint2(int32_t max) {
    return static_cast<int32_t>( rint(static_cast<uint32_t>(max)) );
  }

  template<typename T>
  T rint(T min, T max) {
    return (rint(max - min) + min);
  }

  template<typename T>
  T rint2(T min, T max) {
    return (rint2(max - min) + min);
  }

  bool rbool() {
    // Lowest bits can be lower quality for some engines, so use highest bit.
    // For most architectures, this is compiled to a shift, which should be the same
    // speed as a logical and.
    return ((int64_t) engine()) < 0;
  }

  uint8_t rbyte() {
    return (uint8_t) engine();
  }

  double rdouble() {
    return (engine() >> 11) * 0x1.0p-53;
  }
};

using Rng = SoluxRand<RomuTrio>;

} // end namespace solux