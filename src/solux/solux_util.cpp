#include "solux_util.h"


uint64_t Hash::metro_hash(const void* buffer, int len, uint64_t seed)
{
  auto ptr = (const unsigned char*)buffer;
  auto const end = ptr + len;

  uint64_t h = (seed + k2) * k0;

  if (len >= 32)
  {
    uint64_t v[4];
    v[0] = h;
    v[1] = h;
    v[2] = h;
    v[3] = h;

    do
    {
      v[0] += read_u64(ptr) * k0; ptr += 8; v[0] = rotate_right(v[0],29) + v[2];
      v[1] += read_u64(ptr) * k1; ptr += 8; v[1] = rotate_right(v[1],29) + v[3];
      v[2] += read_u64(ptr) * k2; ptr += 8; v[2] = rotate_right(v[2],29) + v[0];
      v[3] += read_u64(ptr) * k3; ptr += 8; v[3] = rotate_right(v[3],29) + v[1];
    }
    while (ptr <= (end - 32));

    v[2] ^= rotate_right(((v[0] + v[3]) * k0) + v[1], 37) * k1;
    v[3] ^= rotate_right(((v[1] + v[2]) * k1) + v[0], 37) * k0;
    v[0] ^= rotate_right(((v[0] + v[2]) * k0) + v[3], 37) * k1;
    v[1] ^= rotate_right(((v[1] + v[3]) * k1) + v[2], 37) * k0;
    h += v[0] ^ v[1];
  }

  if ((end - ptr) >= 16)
  {
    uint64_t v0 = h + (read_u64(ptr) * k2); ptr += 8; v0 = rotate_right(v0,29) * k3;
    uint64_t v1 = h + (read_u64(ptr) * k2); ptr += 8; v1 = rotate_right(v1,29) * k3;
    v0 ^= rotate_right(v0 * k0, 21) + v1;
    v1 ^= rotate_right(v1 * k3, 21) + v0;
    h += v1;
  }

  if ((end - ptr) >= 8)
  {
    h += read_u64(ptr) * k3; ptr += 8;
    h ^= rotate_right(h, 55) * k1;
  }

  if ((end - ptr) >= 4)
  {
    h += read_u32(ptr) * k3; ptr += 4;
    h ^= rotate_right(h, 26) * k1;
  }

  if ((end - ptr) >= 2)
  {
    h += read_u16(ptr) * k3; ptr += 2;
    h ^= rotate_right(h, 48) * k1;
  }

  if ((end - ptr) >= 1)
  {
    h += read_u8(ptr) * k3;
    h ^= rotate_right(h, 37) * k1;
  }

  h ^= rotate_right(h, 28);
  h *= k0;
  h ^= rotate_right(h, 29);

  return h;
}



uint64_t Hash::fasthash64_func(const void *buf, size_t len, uint64_t seed)
{
  const uint64_t    m = 0x880355f21e6d1965ULL;
  auto pos = (const uint64_t *)buf;
  const auto end = pos + (len / 8);
  const unsigned char *pos2;
  uint64_t h = seed ^ (len * m);
  uint64_t v;

  while (pos != end) {
    v  = *pos++;
    h ^= mix(v);
    h *= m;
  }

  pos2 = (const unsigned char*)pos;
  v = 0;

  switch (len & 7) {
    case 7: v ^= (uint64_t)pos2[6] << 48;
    case 6: v ^= (uint64_t)pos2[5] << 40;
    case 5: v ^= (uint64_t)pos2[4] << 32;
    case 4: v ^= (uint64_t)pos2[3] << 24;
    case 3: v ^= (uint64_t)pos2[2] << 16;
    case 2: v ^= (uint64_t)pos2[1] << 8;
    case 1: v ^= (uint64_t)pos2[0];
      h ^= mix(v);
      h *= m;
  }

  return mix(h);
}