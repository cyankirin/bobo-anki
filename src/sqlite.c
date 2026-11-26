#include "sqlite.h"
#include <stdlib.h>
#include <string.h>

static int be16(const uint8_t* p) { return (p[0] << 8) | p[1]; }
static uint32_t be32(const uint8_t* p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static int varint(const uint8_t* p, int max, int64_t* out) {
  int64_t v = 0;
  int i;
  for (i = 0; i < 9 && i < max; i++) {
    if (i == 8) {
      v = (v << 8) | p[i];
      if (out) *out = v;
      return 9;
    }
    v = (v << 7) | (p[i] & 0x7f);
    if (!(p[i] & 0x80)) {
      if (out) *out = v;
      return i + 1;
    }
  }
  if (out) *out = v;
  return i;
}

int sqlite_open(sqlite_db_t* db, const uint8_t* data, size_t size) {
  if (size < 512 || memcmp(data, "SQLite format 3\0", 16) != 0) return -1;
  int ps = be16(data + 16);
  if (ps == 1) ps = 65536;
  if (ps < 512 || (ps & (ps - 1)) != 0) return -1;
  db->data = data;
  db->size = size;
  db->page_size = ps;
  db->usable = ps - data[20];
  db->page_count = (int)(size / (size_t)ps);
  if (db->page_count < 1) return -1;
  return 0;
}

static const uint8_t* page_ptr(sqlite_db_t* db, uint32_t pgno) {
  if (pgno < 1 || pgno > (uint32_t)db->page_count) return NULL;
  return db->data + (size_t)(pgno - 1) * (size_t)db->page_size;
}

static const uint8_t* payload_build(sqlite_db_t* db, const uint8_t* local, int local_len, int total, uint8_t** owned) {
  *owned = NULL;
  if (total <= local_len) return local;
  uint8_t* buf = (uint8_t*)malloc((size_t)total);
  if (!buf) return NULL;
  memcpy(buf, local, (size_t)local_len);
  int got = local_len;
  uint32_t next = be32(local + local_len);
  while (got < total && next) {
    const uint8_t* ov = page_ptr(db, next);
    if (!ov) {
      free(buf);
      return NULL;
    }
    int chunk = db->usable - 4;
    if (chunk > total - got) chunk = total - got;
    memcpy(buf + got, ov + 4, (size_t)chunk);
    got += chunk;
    next = be32(ov);
  }
  if (got < total) {
    free(buf);
    return NULL;
  }
  *owned = buf;
  return buf;
}

int sqlite_rec_init(sqlite_rec_t* r, sqlite_db_t* db, const uint8_t* payload, int len) {
  if (len < 1) return -1;
  int64_t hdr = 0;
  int n = varint(payload, len, &hdr);
  if (hdr < n || hdr > len) return -1;
  r->db = db;
  r->payload = payload;
  r->payload_len = len;
  r->hdr_len = (int)hdr;
  int count = 0;
  int p = n;
  while (p < (int)hdr) {
    int64_t st = 0;
    int k = varint(payload + p, (int)hdr - p, &st);
    if (k <= 0) break;
    p += k;
    count++;
  }
  r->ncols = count;
  return 0;
}

static int serial_len(int64_t t) {
  switch (t) {
    case 0: return 0;
    case 1: return 1;
    case 2: return 2;
    case 3: return 3;
    case 4: return 4;
    case 5: return 6;
    case 6: return 8;
    case 7: return 8;
    case 8:
    case 9: return 0;
    default:
      if (t >= 12) return (int)((t - 12) / 2);
      return 0;
  }
}

int sqlite_rec_get(sqlite_rec_t* r, int idx, sqlite_val_t* v) {
  int p = varint(r->payload, r->payload_len, NULL);
  int data = r->hdr_len;
  for (int i = 0; i < r->ncols; i++) {
    int64_t st = 0;
    int k = varint(r->payload + p, r->hdr_len - p, &st);
    if (k <= 0) return -1;
    p += k;
    int len = serial_len(st);
    if (data + len > r->payload_len) return -1;
    if (i == idx) {
      memset(v, 0, sizeof(*v));
      const uint8_t* d = r->payload + data;
      if (st == 0) {
        v->type = 0;
      } else if (st >= 1 && st <= 6) {
        int64_t x = 0;
        for (int j = 0; j < len; j++) x = (x << 8) | d[j];
        if (len < 8) {
          int64_t sign = (int64_t)1 << (len * 8 - 1);
          if (x & sign) x -= ((int64_t)1 << (len * 8));
        }
        v->type = 1;
        v->i = x;
      } else if (st == 7) {
        uint64_t bits = 0;
        for (int j = 0; j < 8; j++) bits = (bits << 8) | d[j];
        v->type = 4;
        memcpy(&v->d, &bits, 8);
      } else if (st == 8) {
        v->type = 1;
        v->i = 0;
      } else if (st == 9) {
        v->type = 1;
        v->i = 1;
      } else if (st >= 12) {
        v->type = (st % 2 == 0) ? 3 : 2;
        v->text = d;
        v->len = len;
      }
      return 0;
    }
    data += len;
  }
  return -1;
}

/* walk the sqlite_master tree for a table name */
static uint32_t master_find(sqlite_db_t* db, uint32_t pgno, const char* name, int depth) {
  if (depth > 8) return 0;
  const uint8_t* pg = page_ptr(db, pgno);
  if (!pg) return 0;
  /* page 1 starts with the 100-byte database header before its b-tree header */
  const uint8_t* h = pg + (pgno == 1 ? 100 : 0);
  int type = h[0];
  int ncell = be16(h + 3);
  int cbase = type == 0x05 ? 12 : 8;
  int want = (int)strlen(name);
  for (int i = 0; i <= (type == 0x05 ? ncell : ncell - 1); i++) {
    uint32_t child = (i == ncell) ? be32(h + 8) : 0;
    int cell = (i == ncell) ? 0 : be16(h + cbase + i * 2);
    if (cell >= db->usable) continue;
    const uint8_t* c = pg + cell;
    if (type == 0x05) {
      if (i == ncell) child = be32(h + 8); else child = be32(c);
      uint32_t r = master_find(db, child, name, depth + 1);
      if (r) return r;
    } else if (type == 0x0d) {
      int64_t P = 0;
      int a = varint(c, db->usable - cell, &P);
      if (a <= 0) continue;
      int64_t rowid = 0;
      int b = varint(c + a, db->usable - cell - a, &rowid);
      if (b <= 0) continue;
      int maxlocal = db->usable - 35;
      int local = (int)P;
      if (P > maxlocal) {
        int minlocal = ((db->usable - 12) * 32 / 255) - 23;
        local = minlocal + (int)((P - minlocal) % (db->usable - 4));
      }
      uint8_t* owned = NULL;
      const uint8_t* pl = payload_build(db, c + a + b, local, (int)P, &owned);
      if (!pl) continue;
      sqlite_rec_t r;
      if (sqlite_rec_init(&r, db, pl, (int)P) == 0) {
        sqlite_val_t t, nm, rp;
        if (sqlite_rec_get(&r, 0, &t) == 0 && sqlite_rec_get(&r, 1, &nm) == 0 && sqlite_rec_get(&r, 3, &rp) == 0 &&
            t.type == 2 && nm.type == 2 && (int)nm.len == want && memcmp(nm.text, name, (size_t)want) == 0 && rp.type == 1) {
          free(owned);
          return (uint32_t)rp.i;
        }
      }
      free(owned);
    }
  }
  return 0;
}

uint32_t sqlite_table_root(sqlite_db_t* db, const char* name) { return master_find(db, 1, name, 0); }

static int scan_page(sqlite_db_t* db, uint32_t pgno, int (*cb)(int64_t, sqlite_rec_t*, void*), void* ctx, int depth) {
  if (depth > 12) return 0;
  const uint8_t* pg = page_ptr(db, pgno);
  if (!pg) return 0;
  const uint8_t* h = pg + (pgno == 1 ? 100 : 0);
  int type = h[0];
  int ncell = be16(h + 3);
  int cbase = type == 0x05 ? 12 : 8;
  if (type == 0x05) {
    for (int i = 0; i < ncell; i++) {
      int cell = be16(h + cbase + i * 2);
      if (cell + 4 > db->usable) continue;
      uint32_t child = be32(pg + cell);
      if (scan_page(db, child, cb, ctx, depth + 1)) return 1;
    }
    uint32_t right = be32(h + 8);
    if (scan_page(db, right, cb, ctx, depth + 1)) return 1;
    return 0;
  }
  if (type != 0x0d) return 0;
  for (int i = 0; i < ncell; i++) {
    int cell = be16(h + cbase + i * 2);
    if (cell >= db->usable) continue;
    const uint8_t* c = pg + cell;
    int64_t P = 0;
    int a = varint(c, db->usable - cell, &P);
    if (a <= 0) continue;
    int64_t rowid = 0;
    int b = varint(c + a, db->usable - cell - a, &rowid);
    if (b <= 0) continue;
    int maxlocal = db->usable - 35;
    int local = (int)P;
    if (P > maxlocal) {
      int minlocal = ((db->usable - 12) * 32 / 255) - 23;
      local = minlocal + (int)((P - minlocal) % (db->usable - 4));
    }
    uint8_t* owned = NULL;
    const uint8_t* pl = payload_build(db, c + a + b, local, (int)P, &owned);
    if (!pl) continue;
    sqlite_rec_t r;
    if (sqlite_rec_init(&r, db, pl, (int)P) == 0) {
      if (cb(rowid, &r, ctx)) {
        free(owned);
        return 1;
      }
    }
    free(owned);
  }
  return 0;
}

int sqlite_table_scan(sqlite_db_t* db, uint32_t root, int (*cb)(int64_t, sqlite_rec_t*, void*), void* ctx) {
  return scan_page(db, root, cb, ctx, 0);
}
