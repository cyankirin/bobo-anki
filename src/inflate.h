#pragma once
#include <stddef.h>
#include <stdint.h>

/* Decompress a raw DEFLATE stream. Returns 0 on success, -1 on error. */
int inflate_raw(const void* in, size_t in_len, void* out, size_t out_cap, size_t* out_len);
