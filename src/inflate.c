/* Minimal DEFLATE (RFC 1951) decoder. Memory in, memory out, no zlib. */
#include "inflate.h"

typedef struct {
  const uint8_t* p;
  size_t len;
  size_t bitpos;
  int err;
} br_t;

static int br_bits(br_t* b, int need) {
  int v = 0;
  for (int i = 0; i < need; i++) {
    size_t byte = b->bitpos >> 3;
    if (byte >= b->len) {
      b->err = 1;
      return 0;
    }
    int bit = (b->p[byte] >> (b->bitpos & 7)) & 1;
    b->bitpos++;
    v |= bit << i;
  }
  return v;
}

typedef struct {
  int counts[16];
  int symbols[288];
} huff_t;

static void huff_build(huff_t* h, const int* lengths, int n) {
  for (int i = 0; i < 16; i++) h->counts[i] = 0;
  for (int i = 0; i < n; i++) h->counts[lengths[i]]++;
  h->counts[0] = 0;
  int offs[16];
  offs[0] = 0;
  offs[1] = 0;
  for (int i = 1; i < 15; i++) offs[i + 1] = offs[i] + h->counts[i];
  for (int i = 0; i < n; i++)
    if (lengths[i]) h->symbols[offs[lengths[i]]++] = i;
}

static int huff_decode(br_t* b, const huff_t* h) {
  int code = 0, first = 0, index = 0;
  for (int len = 1; len <= 15; len++) {
    code |= br_bits(b, 1);
    if (b->err) return -1;
    int count = h->counts[len];
    if (code - first < count) return h->symbols[index + code - first];
    index += count;
    first = (first + count) << 1;
    code <<= 1;
  }
  return -1;
}

static const int len_base[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const int len_extra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                  2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const int dist_base[30] = {1,    2,    3,    4,    5,    7,     9,     13,    17,  25,
                                  33,   49,   65,   97,   129,  193,   257,   385,   513, 769,
                                  1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const int dist_extra[30] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                   6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

static int inflate_codes(br_t* b, uint8_t* out, size_t cap, size_t* pos, const huff_t* lit, const huff_t* dist) {
  for (;;) {
    int sym = huff_decode(b, lit);
    if (sym < 0) return -1;
    if (sym < 256) {
      if (*pos >= cap) return -1;
      out[(*pos)++] = (uint8_t)sym;
    } else if (sym == 256) {
      return 0;
    } else {
      sym -= 257;
      if (sym >= 29) return -1;
      int len = len_base[sym] + br_bits(b, len_extra[sym]);
      int dsym = huff_decode(b, dist);
      if (dsym < 0 || dsym >= 30) return -1;
      int d = dist_base[dsym] + br_bits(b, dist_extra[dsym]);
      if (b->err || d <= 0 || (size_t)d > *pos) return -1;
      if (*pos + (size_t)len > cap) return -1;
      for (int i = 0; i < len; i++) {
        out[*pos] = out[*pos - (size_t)d];
        (*pos)++;
      }
    }
  }
}

int inflate_raw(const void* in, size_t in_len, void* out, size_t out_cap, size_t* out_len) {
  br_t b = {(const uint8_t*)in, in_len, 0, 0};
  uint8_t* o = (uint8_t*)out;
  size_t pos = 0;
  int final = 0;
  while (!final) {
    final = br_bits(&b, 1);
    int type = br_bits(&b, 2);
    if (b.err) return -1;
    if (type == 0) { /* stored */
      b.bitpos = (b.bitpos + 7) & ~(size_t)7;
      size_t byte = b.bitpos >> 3;
      if (byte + 4 > b.len) return -1;
      int len = b.p[byte] | (b.p[byte + 1] << 8);
      int nlen = b.p[byte + 2] | (b.p[byte + 3] << 8);
      if ((len ^ 0xffff) != nlen) return -1;
      byte += 4;
      if (byte + (size_t)len > b.len) return -1;
      if (pos + (size_t)len > out_cap) return -1;
      for (int i = 0; i < len; i++) o[pos++] = b.p[byte + (size_t)i];
      b.bitpos = (byte + (size_t)len) << 3;
    } else if (type == 1) { /* fixed */
      int lengths[288];
      for (int i = 0; i < 144; i++) lengths[i] = 8;
      for (int i = 144; i < 256; i++) lengths[i] = 9;
      for (int i = 256; i < 280; i++) lengths[i] = 7;
      for (int i = 280; i < 288; i++) lengths[i] = 8;
      huff_t lit, dist;
      huff_build(&lit, lengths, 288);
      int dl[30];
      for (int i = 0; i < 30; i++) dl[i] = 5;
      huff_build(&dist, dl, 30);
      if (inflate_codes(&b, o, out_cap, &pos, &lit, &dist) != 0) return -1;
    } else if (type == 2) { /* dynamic */
      int nlen = br_bits(&b, 5) + 257;
      int ndist = br_bits(&b, 5) + 1;
      int ncode = br_bits(&b, 4) + 4;
      if (b.err || nlen > 286 || ndist > 30) return -1;
      static const int order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
      int cl[19];
      for (int i = 0; i < 19; i++) cl[i] = 0;
      for (int i = 0; i < ncode; i++) cl[order[i]] = br_bits(&b, 3);
      if (b.err) return -1;
      huff_t clh;
      huff_build(&clh, cl, 19);
      int lengths[320];
      int i = 0;
      while (i < nlen + ndist) {
        int sym = huff_decode(&b, &clh);
        if (sym < 0) return -1;
        if (sym < 16) {
          lengths[i++] = sym;
        } else {
          int rep = 0, val = 0;
          if (sym == 16) {
            if (i == 0) return -1;
            val = lengths[i - 1];
            rep = 3 + br_bits(&b, 2);
          } else if (sym == 17) {
            rep = 3 + br_bits(&b, 3);
          } else {
            rep = 11 + br_bits(&b, 7);
          }
          if (b.err || i + rep > nlen + ndist) return -1;
          while (rep--) lengths[i++] = val;
        }
      }
      if (lengths[256] == 0) return -1;
      huff_t lit, dist;
      huff_build(&lit, lengths, nlen);
      huff_build(&dist, lengths + nlen, ndist);
      if (inflate_codes(&b, o, out_cap, &pos, &lit, &dist) != 0) return -1;
    } else {
      return -1;
    }
  }
  if (out_len) *out_len = pos;
  return 0;
}
