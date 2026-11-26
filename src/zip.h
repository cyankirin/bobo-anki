#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct {
  const uint8_t* data;
  size_t size;
} zip_t;

typedef struct {
  uint16_t method; /* 0 store, 8 deflate */
  uint32_t comp_size;
  uint32_t uncomp_size;
  uint32_t local_off;
  char name[96];
} zip_entry_t;

/* 0 on success */
int zip_open(zip_t* z, const uint8_t* data, size_t size);
/* Find an entry by name; 0 found, -1 missing. */
int zip_find(const zip_t* z, const char* name, zip_entry_t* out);
/* Allocate and return the uncompressed bytes (malloc); NULL on error. */
uint8_t* zip_extract(const zip_t* z, const zip_entry_t* e, size_t* out_len);
