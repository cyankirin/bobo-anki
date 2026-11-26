#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct {
  const uint8_t* data;
  size_t size;
  int page_size;
  int usable;
  int page_count;
} sqlite_db_t;

typedef struct {
  int type; /* 0 null, 1 int, 2 text, 3 blob, 4 real */
  int64_t i;
  double d;
  const uint8_t* text;
  int len;
} sqlite_val_t;

typedef struct {
  sqlite_db_t* db;
  const uint8_t* payload;
  int payload_len;
  int hdr_len;
  int ncols;
} sqlite_rec_t;

/* 0 on success */
int sqlite_open(sqlite_db_t* db, const uint8_t* data, size_t size);
/* Root page of a table (0 when missing). */
uint32_t sqlite_table_root(sqlite_db_t* db, const char* name);
/* Walk a table; callback returns nonzero to stop. Returns 0. */
int sqlite_table_scan(sqlite_db_t* db, uint32_t root, int (*cb)(int64_t rowid, sqlite_rec_t* rec, void* ctx), void* ctx);
int sqlite_rec_init(sqlite_rec_t* r, sqlite_db_t* db, const uint8_t* payload, int len);
int sqlite_rec_get(sqlite_rec_t* r, int idx, sqlite_val_t* v);
