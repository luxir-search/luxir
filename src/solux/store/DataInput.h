#pragma once

#include <cstdint>
#include <assert.h>

#include "solux/util/solux_util.h"


// ripped off directly from Lucene
class DataInput {

  static const int SKIP_BUFFER_SIZE = 1024;

  /* This buffer is used to skip over bytes with the default implementation of
   * skipBytes. The reason why we need to use an instance member instead of
   * sharing a single instance across threads is that some delegating
   * implementations of DataInput might want to reuse the provided buffer in
   * order to eg. update the checksum. If we shared the same buffer across
   * threads, then another thread might update the buffer while the checksum is
   * being computed, making it invalid. See LUCENE-5583 for more information.
   */
  // private byte[] skipBuffer;

public:

  template <class Reader>
  static int32_t readVIntReader(Reader& reader) {
    char b = reader.readByte();
    int32_t i = b & 0x7F;
    int shift;
    for (shift = 7; (b & 0x80) != 0; shift += 7) {
      b = reader.readByte();
      i |= (b & 0x7F) << shift;
    }
    assert(shift < sizeof(int));
    return i;
  }

  template <class Iter>
  static int32_t readVInt(Iter& iter) {
    char b = *iter++;
    int32_t i = b & 0x7F;
    int shift;
    for (shift = 7; (b & 0x80) != 0; shift += 7) {
      b = *iter++;
      i |= (b & 0x7F) << shift;
    }
    assert(shift < sizeof(int));
    return i;
  }


  template <class Reader>
  static int64_t readVLongReader(Reader& reader) {
    char b = reader.readByte();
    int64_t i = b & 0x7F;
    int shift;
    for (shift = 7; (b & 0x80) != 0; shift += 7) {
      b = reader.readByte();
      i |= (b & 0x7FL) << shift;
    }
    assert(shift < sizeof(int64_t));
    return i;
  }

  /** Reads and returns a single byte.
   * @see DataOutput#writeByte(byte)
   */
  virtual byte readByte() = 0;

  uint8_t u8() {
    return (uint8_t) readByte();
  }

  /** Reads a specified number of bytes into an array at the specified offset.
   * @param b the array to read bytes into
   * @param offset the offset in the array to start storing bytes
   * @param len the number of bytes to read
   * @see DataOutput#writeBytes(byte[],int)
   */
  virtual void readBytes(char* b, int offset, int len) = 0;

  /** Reads a specified number of bytes into an array at the
   * specified offset with control over whether the read
   * should be buffered (callers who have their own buffer
   * should pass in "false" for useBuffer).  Currently only
   * {@link BufferedIndexInput} respects this parameter.
   * @param b the array to read bytes into
   * @param offset the offset in the array to start storing bytes
   * @param len the number of bytes to read
   * @param useBuffer set to false if the caller will handle
   * buffering.
   * @see DataOutput#writeBytes(byte[],int)
   */
  virtual void readBytes(char* b, int offset, int len, bool useBuffer)
  {
    // Default to ignoring useBuffer entirely
    readBytes(b, offset, len);
  }

  /** Reads two bytes and returns a short.
   * @see DataOutput#writeByte(byte)
   */
   short readShort() {
    return (short) ((u8() <<  8) | u8());
  }

  /** Reads four bytes and returns an int.
   * @see DataOutput#writeInt(int)
   */
   int readInt()  {
    return (u8() << 24) | (u8() << 16)
         | (u8() <<  8) | u8();
  }


  /** Reads an int stored in variable-length format.  Reads between one and
   * five bytes.  Smaller values take fewer bytes.  Negative numbers are not
   * supported.
   * <p>
   * The format is described further in {@link DataOutput#writeVInt(int)}.
   *
   * @see DataOutput#writeVInt(int)
   */
  int readVInt()   {
    // invoke template function
    return readVIntReader(*this);
  }

  /**
   * Read a {@link BitUtil#zigZagDecode(int) zig-zag}-encoded
   * {@link #readVInt() variable-length} integer.
   * @see DataOutput#writeZInt(int)
   */
  /*
  public int readZInt() throws IOException {
    return BitUtil.zigZagDecode(readVInt());
  }
   */

  /** Reads eight bytes and returns a long.
   * @see DataOutput#writeLong(long)
   */
  /*
  public long readLong() throws IOException {
    return (((long)readInt()) << 32) | (readInt() & 0xFFFFFFFFL);
  }
   */

  /** Reads a long stored in variable-length format.  Reads between one and
   * nine bytes.  Smaller values take fewer bytes.  Negative numbers are not
   * supported.
   * <p>
   * The format is described further in {@link DataOutput#writeVInt(int)}.
   *
   * @see DataOutput#writeVLong(long)
   */
  /*
  public long readVLong() throws IOException {
    return readVLong(false);
  }
   */

  int64_t readVLong() {
    return readVLongReader(*this);
  }

  /**
   * Read a {@link BitUtil#zigZagDecode(long) zig-zag}-encoded
   * {@link #readVLong() variable-length} integer. Reads between one and ten
   * bytes.
   * @see DataOutput#writeZLong(long)
   */
  /*
  public long readZLong() throws IOException {
    return BitUtil.zigZagDecode(readVLong(true));
  }
   */

  /** Reads a string.
   * @see DataOutput#writeString(String)
   */
  /*
  public String readString() throws IOException {
    int length = readVInt();
    final byte[] bytes = new byte[length];
    readBytes(bytes, 0, length);
    return new String(bytes, 0, length, StandardCharsets.UTF_8);
  }
   */

  /** Returns a clone of this stream.
   *
   * <p>Clones of a stream access the same data, and are positioned at the same
   * point as the stream they were cloned from.
   *
   * <p>Expert: Subclasses must ensure that clones may be positioned at
   * different points in the input from each other and from the stream they
   * were cloned from.
   */
  /*
  @Override
  public DataInput clone() {
    try {
      return (DataInput) super.clone();
    } catch (CloneNotSupportedException e) {
      throw new Error("This cannot happen: Failing to clone DataInput");
    }
  }
   */


  /**
   * Reads a Map&lt;String,String&gt; previously written
   * with {@link DataOutput#writeMapOfStrings(Map)}.
   * @return An immutable map containing the written contents.
   */
  /*
  public Map<String,String> readMapOfStrings() throws IOException {
    int count = readVInt();
    if (count == 0) {
      return Collections.emptyMap();
    } else if (count == 1) {
      return Collections.singletonMap(readString(), readString());
    } else {
      Map<String,String> map = count > 10 ? new HashMap<>() : new TreeMap<>();
      for (int i = 0; i < count; i++) {
        final String key = readString();
        final String val = readString();
        map.put(key, val);
      }
      return Collections.unmodifiableMap(map);
    }
  }
*/




  /**
   * Reads a Set&lt;String&gt; previously written
   * with {@link DataOutput#writeSetOfStrings(Set)}.
   * @return An immutable set containing the written contents.
   */
  /*
  public Set<String> readSetOfStrings() throws IOException {
    int count = readVInt();
    if (count == 0) {
      return Collections.emptySet();
    } else if (count == 1) {
      return Collections.singleton(readString());
    } else {
      Set<String> set = count > 10 ? new HashSet<>() : new TreeSet<>();
      for (int i = 0; i < count; i++) {
        set.add(readString());
      }
      return Collections.unmodifiableSet(set);
    }
  }
   */

  /**
   * Skip over <code>numBytes</code> bytes. The contract on this method is that it
   * should have the same behavior as reading the same number of bytes into a
   * buffer and discarding its content. Negative values of <code>numBytes</code>
   * are not supported.
   */
  /**
  virtual void skipBytes(int64_t numBytes) {
    //if (numBytes < 0) {
    //  throw new IllegalArgumentException("numBytes must be >= 0, got " + numBytes);
    //}
    if (skipBuffer == null) {
      skipBuffer = new byte[SKIP_BUFFER_SIZE];
    }
    // assert skipBuffer.length == SKIP_BUFFER_SIZE;
    for (int64_t skipped = 0; skipped < numBytes; ) {
      final int step = (int) Math.min(SKIP_BUFFER_SIZE, numBytes - skipped);
      readBytes(skipBuffer, 0, step, false);
      skipped += step;
    }
  }
   **/

};

