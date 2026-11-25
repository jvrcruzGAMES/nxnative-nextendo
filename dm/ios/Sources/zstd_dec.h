#ifndef DM_ZSTD_DEC_H
#define DM_ZSTD_DEC_H

#include <stddef.h>

// Decompress a Zstandard (.zst) file at in_path to out_path.
// Returns 0 on success, -1 on error (writes a brief reason to err_buf if non-NULL).
int dm_zstd_decompress_file(const char *in_path, const char *out_path, char *err_buf, size_t err_cap);

#endif
