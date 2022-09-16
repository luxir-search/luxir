#pragma once


// Most x86_64 processors only use the lower 48 bits of pointers.  The upper bits (48 through 63) must all be 1 or 0
// and must match the sign of the 47th bit (the processor will throw an exception if this is not the case.)
// This has been extended with 5-level page tables to 57 bits of virtual address space (leaving 7 bits)!
// Bits 63-57 must match bit 56, and the way most operating systems are implemented, bit 56 of a user-space
// pointer should always be 0, so we could get 8 bits if really needed.
template <class T>
class TaggedPtr {
  int64_t x;
public:
  static const uint32_t TAG_BITS = 7;
  static const uint32_t MAX_TAG = (1 << TAG_BITS) - 1;

  TaggedPtr() {}

  TaggedPtr(T *ptr, uint32_t tag) {
    x = (reinterpret_cast<int64_t>(ptr) << TAG_BITS) + tag;
    assert(tag <= MAX_TAG && this->ptr() == ptr);
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
    // must match the 48th bit on x86, other architectures can ignore those bits).
    // Most operating systems I know of use the "0" half of the address
    // space for user-space, but the signed extension is free anyway for our purposes here.
    return reinterpret_cast<T *>(x >> TAG_BITS);
  }

  uint32_t tag() const { return x & MAX_TAG; }

  uint64_t packedBits() const { return x; }

  void* packedBitsAsVoid() const { return reinterpret_cast<void*>(x); }
};
