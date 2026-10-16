/*
 * SADES Engine — a reimplementation
 * Copyright (c) 2026 Kalizkal
 * MIT License — see LICENSE
 */

#ifndef SADES_M00_H
#define SADES_M00_H

#include <stdint.h>
#include <stddef.h>

/* Loader for the SADES .M00 message archive.
 *
 * A .M00 file is a flat sequence of Shift-JIS strings terminated by
 * single NUL bytes. The VM's CALL opcode (0x40) refers to strings by
 * absolute file offset; there is no header, index, or lookup table.
 *
 * Layout:
 *     <string 1> \0
 *     <string 2> \0
 *     ...
 *     <string N> \0
 *
 * Strings are not aligned or padded. Empty slots are represented by a
 * single NUL byte, so a valid CALL offset can point directly at a NUL
 * and yield an empty string; callers must tolerate that.
 *
 * Character encoding is Shift-JIS with no BOM. Some entries contain
 * the U+25BD character (▽, "wait for input" marker) encoded as
 * Shift-JIS 0x81 0xA4.
 */

typedef struct {
    uint8_t *data;
    size_t   size;
} M00;

/* Load a .M00 file into memory. Returns 0 on success, -1 on error.
 * On success, `m->data` owns a heap buffer that must be released with
 * m00_free(). On failure, `m` is left in a consistent empty state. */
int  m00_load(const char *path, M00 *m);

/* Release the buffer. Safe to call multiple times, and safe to call
 * on a zero-initialized M00 struct. */
void m00_free(M00 *m);

/* Return a pointer to the NUL-terminated string starting at byte
 * offset `off`. `*len` receives the byte length of the string
 * (excluding the terminating NUL).
 *
 *   - If `off` is past the end of the file, returns NULL.
 *   - If `off` points at a NUL byte, returns that byte and sets
 *     `*len` to 0.
 *   - The returned pointer is valid until m00_free() is called; the
 *     caller must not modify or free the underlying buffer.
 *
 * NOTE: the bytes are Shift-JIS, not UTF-8. Convert with
 * sjis_to_utf8() before handing them to the renderer. */
const uint8_t *m00_get(const M00 *m, size_t off, size_t *len);

/* Convert a Shift-JIS byte sequence to UTF-8.
 *
 *   - Writes at most `outlen - 1` bytes plus a trailing NUL.
 *   - Never reads past `in + inlen`.
 *   - Invalid or unmappable sequences are replaced with a single
 *     ASCII '?' and the encoder advances by exactly one byte, so a
 *     malformed sequence cannot desync the rest of the stream.
 *   - A truncated multi-byte character at the end of the input is
 *     likewise replaced with '?'.
 *
 * Returns the number of bytes written to `out` (excluding the NUL),
 * or -1 if the caller passed invalid arguments. */
int  sjis_to_utf8(const uint8_t *in, size_t inlen,
                  char *out, size_t outlen);

#endif /* SADES_M00_H */