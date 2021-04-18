#pragma once


// most x86_64 processors only use the lower 48 bits of pointers.  The upper bits (48 through 63) must all be 1 or 0
// and must match the sign of the 47th bit (the processor will throw an exception if this is not the case)
// But that can be extended to 52 bits in the future (leaving 12 free bits.)
template <class T>
class TaggedPtr {
  int64_t x;
public:
  static const uint32_t TAG_BITS = 8;    // For now, use 8... we don't currently need more
  static const uint32_t MAX_TAG = (1 << TAG_BITS) - 1;

  TaggedPtr() {}

  TaggedPtr(T *ptr, uint32_t tag) {
    assert(tag <= MAX_TAG);
    x = (reinterpret_cast<int64_t>(ptr) << TAG_BITS) + tag;
  }

  explicit TaggedPtr(uint64_t taggedPtr) {
    x = taggedPtr;
  }

  // Recreate a TaggedPtr that has previously been converted to a void*
  // We don't have a constructor that takes a void* because that would be error prone. This is explicit.
  static TaggedPtr fromTaggedPtrBits(void* taggedPtr) {
    return TaggedPtr(reinterpret_cast<int64_t>(taggedPtr));
  }

  T* ptr() const {
    // Do a signed shift so we get the correct sign extension (all bits above the 48th bit
    // must match the 48th bit).  Most operating systems I know of use the "0" half of the address
    // space for user-space, but the signed extension is free anyway for our purposes here (unless
    // directly storing the top 16 bits is cheaper since no shift is needed?)
    return reinterpret_cast<T *>(x >> TAG_BITS);
  }

  uint32_t tag() const { return x & MAX_TAG; }

  uint64_t packedBits() const { return x; }

  void* packedBitsAsVoid() const { return reinterpret_cast<void*>(x); }
};
