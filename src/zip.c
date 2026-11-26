#include "zip.h"
#include "inflate.h"
#include <stdlib.h>
#include <string.h>

static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

int zip_open(zip_t* z, const uint8_t* data, size_t size) {
  z->data = data;
  z->size = size;
  return size < 22 ? -1 : 0;
}

static size_t eocd_offset(const zip_t* z) {
  for (size_t i = z->size - 22; i + 22 <= z->size; i--) {
    const uint8_t* d = z->data;
    if (d[i] == 'P' && d[i + 1] == 'K' && d[i + 2] == 5 && d[i + 3] == 6) return i;
    if (i == 0) break;
  }
  return (size_t)-1;
}

static int find_local(const zip_t* z, const char* name, zip_entry_t* out) {
  size_t p = 0, want = strlen(name);
  while (p + 30 <= z->size) {
    const uint8_t* h = z->data + p;
    if (!(h[0] == 'P' && h[1] == 'K' && h[2] == 3 && h[3] == 4)) return -1;
    uint16_t method = rd16(h + 8);
    uint32_t csize = rd32(h + 18);
    uint32_t usize = rd32(h + 22);
    uint16_t nlen = rd16(h + 26);
    uint16_t xlen = rd16(h + 28);
    if (p + 30 + nlen > z->size) return -1;
    const char* en = (const char*)(h + 30);
    if (nlen == want && strncmp(en, name, nlen) == 0) {
      out->method = method;
      out->comp_size = csize;
      out->uncomp_size = usize;
      out->local_off = (uint32_t)p;
      size_t n = nlen < sizeof(out->name) - 1 ? nlen : sizeof(out->name) - 1;
      memcpy(out->name, en, n);
      out->name[n] = 0;
      return 0;
    }
    if (csize == 0 && usize != 0) return -1; /* data descriptor: cannot walk further */
    p += 30 + nlen + xlen + csize;
  }
  return -1;
}

int zip_find(const zip_t* z, const char* name, zip_entry_t* out) {
  size_t eo = eocd_offset(z);
  if (eo == (size_t)-1 || eo + 22 > z->size) return find_local(z, name, out);
  uint16_t count = rd16(z->data + eo + 10);
  uint32_t cd_off = rd32(z->data + eo + 16);
  size_t p = cd_off;
  for (uint16_t i = 0; i < count; i++) {
    if (p + 46 > z->size) return -1;
    const uint8_t* h = z->data + p;
    if (!(h[0] == 'P' && h[1] == 'K' && h[2] == 1 && h[3] == 2)) return -1;
    uint16_t method = rd16(h + 10);
    uint32_t csize = rd32(h + 20);
    uint32_t usize = rd32(h + 24);
    uint16_t nlen = rd16(h + 28);
    uint16_t xlen = rd16(h + 30);
    uint16_t clen = rd16(h + 32);
    uint32_t loff = rd32(h + 42);
    if (p + 46 + nlen > z->size) return -1;
    const char* ename = (const char*)(h + 46);
    if (strlen(name) == nlen && strncmp(ename, name, nlen) == 0) {
      out->method = method;
      out->comp_size = csize;
      out->uncomp_size = usize;
      out->local_off = loff;
      size_t n = nlen < sizeof(out->name) - 1 ? nlen : sizeof(out->name) - 1;
      memcpy(out->name, ename, n);
      out->name[n] = 0;
      return 0;
    }
    p += 46 + nlen + xlen + clen;
  }
  return -1;
}

uint8_t* zip_extract(const zip_t* z, const zip_entry_t* e, size_t* out_len) {
  size_t p = e->local_off;
  if (p + 30 > z->size) return NULL;
  const uint8_t* h = z->data + p;
  if (!(h[0] == 'P' && h[1] == 'K' && h[2] == 3 && h[3] == 4)) return NULL;
  uint16_t nlen = rd16(h + 26);
  uint16_t xlen = rd16(h + 28);
  size_t data_off = p + 30 + nlen + xlen;
  if (data_off + e->comp_size > z->size) return NULL;
  if (e->uncomp_size > (4u << 20)) return NULL; /* refuse >8 MB */
  uint8_t* out = (uint8_t*)malloc(e->uncomp_size ? e->uncomp_size : 1);
  if (!out) return NULL;
  if (e->method == 0) {
    if (e->comp_size < e->uncomp_size) {
      free(out);
      return NULL;
    }
    memcpy(out, z->data + data_off, e->uncomp_size);
  } else if (e->method == 8) {
    size_t got = 0;
    if (inflate_raw(z->data + data_off, e->comp_size, out, e->uncomp_size, &got) != 0 || got != e->uncomp_size) {
      free(out);
      return NULL;
    }
  } else {
    free(out);
    return NULL;
  }
  if (out_len) *out_len = e->uncomp_size;
  return out;
}
