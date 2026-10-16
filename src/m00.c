/*
 * SADES Engine — a reimplementation
 * Copyright (c) 2026 Kalizkal
 * MIT License — see LICENSE
 */

#include "m00.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <iconv.h>

/* --------------------------------------------------------------------- */
/* File loading                                                          */
/* --------------------------------------------------------------------- */

int m00_load(const char *path, M00 *m) {
    if (!m) return -1;
    m->data = NULL;
    m->size = 0;

    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return -1; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return -1; }

    if (sz == 0) {
        /* Empty file is legal. Every lookup will return NULL. */
        fclose(f);
        return 0;
    }

    uint8_t *buf = malloc((size_t)sz);
    if (!buf) { fclose(f); return -1; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf);
        fclose(f);
        return -1;
    }
    fclose(f);

    m->data = buf;
    m->size = (size_t)sz;
    return 0;
}

void m00_free(M00 *m) {
    if (!m) return;
    free(m->data);
    m->data = NULL;
    m->size = 0;
}

/* --------------------------------------------------------------------- */
/* String lookup                                                         */
/* --------------------------------------------------------------------- */

const uint8_t *m00_get(const M00 *m, size_t off, size_t *len) {
    if (len) *len = 0;
    if (!m || !m->data) return NULL;
    if (off >= m->size) return NULL;

    const uint8_t *start = m->data + off;
    const uint8_t *p     = start;
    const uint8_t *end   = m->data + m->size;

    while (p < end && *p != 0) p++;

    if (len) *len = (size_t)(p - start);
    return start;
}

/* --------------------------------------------------------------------- */
/* Shift-JIS -> UTF-8                                                    */
/* --------------------------------------------------------------------- */

/*
 * iconv descriptors are stateful and not thread-safe. The VM is
 * strictly single-threaded, so we keep one around and reset it
 * between conversions.
 */
static iconv_t g_cd = (iconv_t)-1;

static int ensure_iconv(void) {
    if (g_cd != (iconv_t)-1) return 0;
    g_cd = iconv_open("UTF-8", "SHIFT-JIS");
    return (g_cd == (iconv_t)-1) ? -1 : 0;
}

int sjis_to_utf8(const uint8_t *in, size_t inlen,
                 char *out, size_t outlen) {
    if (!out || outlen == 0) return -1;
    if (!in || inlen == 0) { out[0] = 0; return 0; }

    if (ensure_iconv() != 0) {
        /* No iconv available. Emit '?' for each input byte so the
         * caller still gets a printable string, rather than silently
         * pretending to understand the encoding. */
        size_t n = (inlen < outlen - 1) ? inlen : (outlen - 1);
        memset(out, '?', n);
        out[n] = 0;
        return (int)n;
    }

    /* Reset any state left over from a previous aborted conversion. */
    iconv(g_cd, NULL, NULL, NULL, NULL);

    /* iconv's signature isn't const-correct. */
    char  *pin      = (char *)(uintptr_t)in;
    size_t in_left  = inlen;
    char  *pout     = out;
    size_t out_left = outlen - 1;              /* reserve room for NUL */

    while (in_left > 0 && out_left > 0) {
        if (iconv(g_cd, &pin, &in_left, &pout, &out_left) != (size_t)-1) {
            continue;
        }
        if (errno == E2BIG) {
            /* Output full. Stop cleanly with what we have. */
            break;
        }
        if (errno == EILSEQ) {
            /* Invalid byte sequence: emit '?' and advance one byte so
             * the rest of the string can still be decoded. */
            *pout++ = '?';
            out_left--;
            pin++;
            in_left--;
            continue;
        }
        if (errno == EINVAL) {
            /* Truncated multi-byte character at the end of input. */
            *pout++ = '?';
            out_left--;
            break;
        }
        /* Something unexpected. Bail out without corrupting output. */
        break;
    }

    *pout = 0;
    return (int)(pout - out);
}