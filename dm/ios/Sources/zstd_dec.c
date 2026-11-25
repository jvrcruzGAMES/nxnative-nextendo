// Streaming RFC 8878 Zstandard file-to-file decompressor for NxShare v3.
#include "zstd_dec.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ZSTD_MAGIC 0xFD2FB528u
#define ZSTD_SKIP_MASK 0xFFFFFFF0u
#define ZSTD_SKIP_MAGIC 0x184D2A50u
#define MAX_BLOCK_SIZE (128u * 1024u)

typedef struct {
    uint16_t new_state;
    uint8_t nb_bits;
    uint8_t symbol;
} fse_entry;

typedef struct {
    fse_entry *table;
    int table_log;
} fse_table;

typedef struct {
    uint8_t symbol;
    uint8_t nb_bits;
} huf_entry;

typedef struct {
    huf_entry *table;
    int max_bits;
} huf_table;

static int set_err(char *err_buf, size_t err_cap, const char *msg) {
    if (err_buf && err_cap) snprintf(err_buf, err_cap, "%s", msg);
    return -1;
}

static inline uint16_t rd16(const uint8_t *p) { return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }
static inline uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static inline uint64_t rd64(const uint8_t *p) { return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32); }

// Clean, exact backwards bit reader (RFC 8878 Section 4.2.1.2):
typedef struct {
    const uint8_t *src;
    size_t bit_offset;   // bits remaining from start of src
} bw_reader;

static int bw_init(bw_reader *r, const uint8_t *src, size_t len) {
    if (!len || src[len - 1] == 0) return -1;
    uint8_t last = src[len - 1];
    int hb = 7;
    while (((last >> hb) & 1) == 0) hb--;
    r->src = src;
    r->bit_offset = (len - 1) * 8 + (size_t)hb;
    return 0;
}

static inline uint32_t bw_read(bw_reader *r, int nbits) {
    if (nbits == 0) return 0;
    if ((size_t)nbits > r->bit_offset) {
        // Zero-pad if reading past start (only valid for final HUF padding? No, RFC 8878 allows padding on HUF)
        size_t have = r->bit_offset;
        uint32_t v = 0;
        for (size_t i = 0; i < have; i++) {
            size_t bpos = r->bit_offset - 1 - i;
            v = (v << 1) | ((r->src[bpos >> 3] >> (bpos & 7)) & 1u);
        }
        r->bit_offset = 0;
        return v << (nbits - (int)have);
    }
    size_t end_bit = r->bit_offset;
    size_t start_bit = end_bit - (size_t)nbits;
    r->bit_offset = start_bit;
    size_t byte_idx = start_bit >> 3;
    int bit_in_byte = (int)(start_bit & 7);
    uint64_t w = 0;
    size_t bytes_needed = ((size_t)bit_in_byte + (size_t)nbits + 7) >> 3;
    for (size_t i = 0; i < bytes_needed; i++) w |= (uint64_t)r->src[byte_idx + i] << (i * 8);
    uint64_t mask = (nbits == 32) ? 0xFFFFFFFFull : ((1ull << nbits) - 1ull);
    return (uint32_t)((w >> bit_in_byte) & mask);
}

static inline uint32_t bw_peek(const bw_reader *r, int nbits) {
    bw_reader tmp = *r;
    return bw_read(&tmp, nbits);
}

// Forward bit reader for FSE table headers
typedef struct {
    const uint8_t *src;
    size_t len;
    size_t bit_pos;
} fw_reader;

static inline uint32_t fw_read(fw_reader *r, int nbits) {
    if (nbits == 0) return 0;
    uint32_t val = 0;
    for (int i = 0; i < nbits; i++) {
        size_t bp = r->bit_pos++;
        if ((bp >> 3) < r->len) {
            val |= (uint32_t)((r->src[bp >> 3] >> (bp & 7)) & 1u) << i;
        }
    }
    return val;
}

static int fse_build(fse_table *t, const int16_t *norm, unsigned max_sym, int table_log) {
    int table_size = 1 << table_log;
    free(t->table);
    t->table = (fse_entry *)calloc((size_t)table_size, sizeof(fse_entry));
    if (!t->table) return -1;
    t->table_log = table_log;

    uint8_t *symbols = (uint8_t *)malloc((size_t)table_size);
    uint16_t *sym_next = (uint16_t *)malloc((max_sym + 1) * sizeof(uint16_t));
    if (!symbols || !sym_next) { free(symbols); free(sym_next); return -1; }

    int high_thresh = table_size - 1;
    for (unsigned s = 0; s <= max_sym; s++) {
        if (norm[s] == -1) {
            symbols[high_thresh--] = (uint8_t)s;
            sym_next[s] = 1;
        } else {
            sym_next[s] = (uint16_t)norm[s];
        }
    }

    int pos = 0;
    int step = (table_size >> 1) + (table_size >> 3) + 3;
    int mask = table_size - 1;
    for (unsigned s = 0; s <= max_sym; s++) {
        for (int i = 0; i < norm[s]; i++) {
            symbols[pos] = (uint8_t)s;
            pos = (pos + step) & mask;
            while (pos > high_thresh) pos = (pos + step) & mask;
        }
    }

    for (int i = 0; i < table_size; i++) {
        uint8_t sym = symbols[i];
        uint16_t next_occ = sym_next[sym]++;
        int nb_bits = table_log - (31 - __builtin_clz((unsigned)next_occ));
        uint16_t new_state = (uint16_t)((next_occ << nb_bits) - (unsigned)table_size);
        t->table[i].symbol = sym;
        t->table[i].nb_bits = (uint8_t)nb_bits;
        t->table[i].new_state = new_state;
    }

    free(symbols);
    free(sym_next);
    return 0;
}

static int fse_read_ncount(const uint8_t *src, size_t len, int16_t *norm, unsigned *max_sym_out, int *table_log_out, size_t *bytes_read) {
    if (len < 1) return -1;
    fw_reader r = {src, len, 0};
    int table_log = (int)fw_read(&r, 4) + 5;
    if (table_log > 15) return -1;
    int remaining = (1 << table_log) + 1;
    int threshold = 1 << table_log;
    int nb_bits = table_log + 1;
    unsigned sym = 0;
    int prev_zero = 0;

    memset(norm, 0, 256 * sizeof(int16_t));
    while (remaining > 1 && sym < 256) {
        if (prev_zero) {
            unsigned n0 = sym;
            while (1) {
                uint32_t rep = fw_read(&r, 2);
                n0 += rep;
                if (rep < 3) break;
            }
            if (n0 > 255) return -1;
            sym = n0;
            prev_zero = 0;
            continue;
        }
        int max_val = (2 * threshold - 1) - remaining;
        uint32_t low = fw_read(&r, nb_bits - 1);
        int val;
        if ((int)low < max_val) {
            val = (int)low;
        } else {
            uint32_t hi = fw_read(&r, 1);
            val = (int)(low + (hi << (nb_bits - 1)));
            if (val >= threshold) val -= max_val;
        }
        val--;
        remaining -= (val < 0) ? -val : val;
        norm[sym++] = (int16_t)val;
        prev_zero = (val == 0);
        while (remaining < threshold) {
            nb_bits--;
            threshold >>= 1;
        }
    }
    if (remaining != 1) return -1;
    *max_sym_out = sym - 1;
    *table_log_out = table_log;
    *bytes_read = (r.bit_pos + 7) >> 3;
    return (*bytes_read <= len) ? 0 : -1;
}

static int fse_build_predefined_ll(fse_table *t) {
    static const int16_t ll[36] = {
        4,3,2,2,2,2,2,2,2,2,2,2,2,1,1,1,2,2,2,2,2,2,2,2,2,3,2,1,1,1,1,1,-1,-1,-1,-1
    };
    return fse_build(t, ll, 35, 6);
}
static int fse_build_predefined_of(fse_table *t) {
    static const int16_t of[29] = {
        1,1,1,1,1,1,2,2,2,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,-1,-1,-1,-1,-1
    };
    return fse_build(t, of, 28, 5);
}
static int fse_build_predefined_ml(fse_table *t) {
    static const int16_t ml[53] = {
        1,4,3,2,2,2,2,2,2,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,-1,-1,-1,-1,-1,-1,-1
    };
    return fse_build(t, ml, 52, 6);
}

static int fse_build_rle(fse_table *t, uint8_t sym) {
    free(t->table);
    t->table = (fse_entry *)calloc(1, sizeof(fse_entry));
    if (!t->table) return -1;
    t->table_log = 0;
    t->table[0].symbol = sym;
    t->table[0].nb_bits = 0;
    t->table[0].new_state = 0;
    return 0;
}

static inline uint8_t fse_decode_sym(const fse_table *t, uint16_t *state, bw_reader *br) {
    fse_entry e = t->table[*state];
    *state = (uint16_t)(e.new_state + bw_read(br, e.nb_bits));
    return e.symbol;
}

static int huf_read_weights(const uint8_t *src, size_t len, uint8_t *weights, unsigned *num_syms, size_t *bytes_consumed) {
    if (len < 1) return -1;
    uint8_t hdr = src[0];
    if (hdr >= 128) {
        unsigned n = (unsigned)(hdr - 127);
        size_t need = 1 + ((n + 1) / 2);
        if (len < need) return -1;
        for (unsigned i = 0; i < n; i += 2) {
            uint8_t b = src[1 + (i / 2)];
            weights[i] = b >> 4;
            if (i + 1 < n) weights[i + 1] = b & 0x0F;
        }
        *num_syms = n;
        *bytes_consumed = need;
        return 0;
    }
    size_t comp_size = hdr;
    if (len < 1 + comp_size) return -1;
    const uint8_t *p = src + 1;
    int16_t norm[256];
    unsigned max_sym = 0;
    int tlog = 0;
    size_t fse_hdr_bytes = 0;
    if (fse_read_ncount(p, comp_size, norm, &max_sym, &tlog, &fse_hdr_bytes) < 0) return -1;
    fse_table ft = {0};
    if (fse_build(&ft, norm, max_sym, tlog) < 0) { free(ft.table); return -1; }
    bw_reader br;
    if (bw_init(&br, p + fse_hdr_bytes, comp_size - fse_hdr_bytes) < 0) { free(ft.table); return -1; }
    uint16_t s1 = (uint16_t)bw_read(&br, tlog);
    uint16_t s2 = (uint16_t)bw_read(&br, tlog);
    unsigned n = 0;
    while (n < 255) {
        fse_entry e1 = ft.table[s1];
        weights[n++] = e1.symbol;
        if ((size_t)e1.nb_bits > br.bit_offset) {
            weights[n++] = ft.table[s2].symbol;
            break;
        }
        s1 = (uint16_t)(e1.new_state + bw_read(&br, e1.nb_bits));
        fse_entry e2 = ft.table[s2];
        weights[n++] = e2.symbol;
        if ((size_t)e2.nb_bits > br.bit_offset) {
            weights[n++] = ft.table[s1].symbol;
            break;
        }
        s2 = (uint16_t)(e2.new_state + bw_read(&br, e2.nb_bits));
    }
    free(ft.table);
    *num_syms = n;
    *bytes_consumed = 1 + comp_size;
    return 0;
}

static int huf_build(huf_table *h, const uint8_t *weights, unsigned n_minus_1) {
    uint64_t sum = 0;
    for (unsigned i = 0; i < n_minus_1; i++) {
        if (weights[i] > 0) sum += 1ull << (weights[i] - 1);
    }
    if (sum == 0) return -1;
    int max_bits = 1;
    while ((1ull << max_bits) <= sum) max_bits++;
    if (max_bits > 12) return -1;
    uint64_t left = (1ull << max_bits) - sum;
    if ((left & (left - 1)) != 0) return -1;
    int last_w = 1;
    while ((1ull << (last_w - 1)) < left) last_w++;

    uint8_t all_w[256];
    memcpy(all_w, weights, n_minus_1);
    all_w[n_minus_1] = (uint8_t)last_w;
    unsigned total_syms = n_minus_1 + 1;

    uint8_t nbits[256];
    uint16_t rank_count[13] = {0};
    for (unsigned i = 0; i < total_syms; i++) {
        nbits[i] = all_w[i] ? (uint8_t)(max_bits + 1 - all_w[i]) : 0;
        if (nbits[i] > 0) rank_count[nbits[i]]++;
    }

    uint16_t rank_start[13] = {0};
    uint16_t cur = 0;
    for (int b = max_bits; b >= 1; b--) {
        rank_start[b] = cur;
        cur += rank_count[b] << (max_bits - b);
    }

    int tsize = 1 << max_bits;
    free(h->table);
    h->table = (huf_entry *)calloc((size_t)tsize, sizeof(huf_entry));
    if (!h->table) return -1;
    h->max_bits = max_bits;

    for (unsigned s = 0; s < total_syms; s++) {
        uint8_t nb = nbits[s];
        if (!nb) continue;
        uint16_t code = rank_start[nb];
        int span = 1 << (max_bits - nb);
        rank_start[nb] += (uint16_t)span;
        for (int j = 0; j < span; j++) {
            h->table[code + j].symbol = (uint8_t)s;
            h->table[code + j].nb_bits = nb;
        }
    }
    return 0;
}

static int huf_decode_1stream(const huf_table *h, const uint8_t *src, size_t src_len, uint8_t *dst, size_t dst_len) {
    bw_reader br;
    if (bw_init(&br, src, src_len) < 0) return -1;
    int mb = h->max_bits;
    for (size_t i = 0; i < dst_len; i++) {
        uint32_t code = bw_peek(&br, mb);
        huf_entry e = h->table[code];
        if (e.nb_bits == 0) return -1;
        bw_read(&br, e.nb_bits);
        dst[i] = e.symbol;
    }
    return 0;
}

static int huf_decode_4stream(const huf_table *h, const uint8_t *src, size_t src_len, uint8_t *dst, size_t dst_len) {
    if (src_len < 6) return -1;
    size_t s1 = rd16(src);
    size_t s2 = rd16(src + 2);
    size_t s3 = rd16(src + 4);
    if (6 + s1 + s2 + s3 > src_len) return -1;
    size_t s4 = src_len - 6 - s1 - s2 - s3;
    size_t seg = (dst_len + 3) / 4;
    if (seg * 3 > dst_len) return -1;
    const uint8_t *p1 = src + 6;
    const uint8_t *p2 = p1 + s1;
    const uint8_t *p3 = p2 + s2;
    const uint8_t *p4 = p3 + s3;
    if (huf_decode_1stream(h, p1, s1, dst, seg) < 0) return -1;
    if (huf_decode_1stream(h, p2, s2, dst + seg, seg) < 0) return -1;
    if (huf_decode_1stream(h, p3, s3, dst + seg * 2, seg) < 0) return -1;
    if (huf_decode_1stream(h, p4, s4, dst + seg * 3, dst_len - seg * 3) < 0) return -1;
    return 0;
}

typedef struct {
    fse_table ll_tab, of_tab, ml_tab;
    huf_table huf;
    int has_huf, has_ll, has_of, has_ml;
    uint32_t rep[3];
    uint8_t *win;
    size_t win_size;
    size_t win_pos;
    uint64_t total_out;
    FILE *out;
} zstd_ctx;

static int win_emit(zstd_ctx *c, const uint8_t *data, size_t len) {
    if (len == 0) return 0;
    if (fwrite(data, 1, len, c->out) != len) return -1;
    if (len >= c->win_size) {
        memcpy(c->win, data + (len - c->win_size), c->win_size);
        c->win_pos = 0;
    } else {
        size_t first = c->win_size - c->win_pos;
        if (len <= first) {
            memcpy(c->win + c->win_pos, data, len);
            c->win_pos = (c->win_pos + len) % c->win_size;
        } else {
            memcpy(c->win + c->win_pos, data, first);
            memcpy(c->win, data + first, len - first);
            c->win_pos = len - first;
        }
    }
    c->total_out += len;
    return 0;
}

static int win_emit_byte(zstd_ctx *c, uint8_t b, size_t count) {
    uint8_t buf[4096];
    memset(buf, b, sizeof(buf));
    while (count > 0) {
        size_t n = count < sizeof(buf) ? count : sizeof(buf);
        if (win_emit(c, buf, n) < 0) return -1;
        count -= n;
    }
    return 0;
}

static int win_copy_match(zstd_ctx *c, uint32_t offset, size_t match_len) {
    if (offset == 0 || (uint64_t)offset > c->total_out || (size_t)offset > c->win_size) return -1;
    uint8_t tmp[4096];
    while (match_len > 0) {
        size_t chunk = match_len < sizeof(tmp) ? match_len : sizeof(tmp);
        if (chunk > (size_t)offset) chunk = (size_t)offset;
        size_t src_pos = (c->win_pos + c->win_size - (size_t)offset) % c->win_size;
        size_t first = c->win_size - src_pos;
        if (chunk <= first) {
            memcpy(tmp, c->win + src_pos, chunk);
        } else {
            memcpy(tmp, c->win + src_pos, first);
            memcpy(tmp + first, c->win, chunk - first);
        }
        if (win_emit(c, tmp, chunk) < 0) return -1;
        match_len -= chunk;
    }
    return 0;
}

static int decode_compressed_block(zstd_ctx *c, const uint8_t *blk, size_t blk_len) {
    if (blk_len < 1) return -1;
    size_t pos = 0;
    uint8_t lh0 = blk[pos++];
    uint8_t ltype = lh0 & 3;
    uint8_t size_fmt = (lh0 >> 2) & 3;
    size_t reg_size = 0, comp_size = 0;

    if (ltype == 0 || ltype == 1) {
        if ((size_fmt & 1) == 0) reg_size = lh0 >> 3;
        else if (size_fmt == 1) { if (pos >= blk_len) return -1; reg_size = (lh0 >> 4) + ((size_t)blk[pos++] << 4); }
        else { if (pos + 1 >= blk_len) return -1; reg_size = (lh0 >> 4) + ((size_t)blk[pos] << 4) + ((size_t)blk[pos + 1] << 12); pos += 2; }
    } else {
        if (size_fmt == 0 || size_fmt == 1) {
            if (pos + 1 >= blk_len) return -1;
            uint32_t v = (lh0 >> 4) | ((uint32_t)blk[pos] << 4) | ((uint32_t)blk[pos + 1] << 12);
            pos += 2;
            reg_size = v & 0x3FF;
            comp_size = (v >> 10) & 0x3FF;
        } else if (size_fmt == 2) {
            if (pos + 2 >= blk_len) return -1;
            uint32_t v = (lh0 >> 4) | ((uint32_t)blk[pos] << 4) | ((uint32_t)blk[pos + 1] << 12) | ((uint32_t)blk[pos + 2] << 20);
            pos += 3;
            reg_size = v & 0x3FFF;
            comp_size = (v >> 14) & 0x3FFF;
        } else {
            if (pos + 3 >= blk_len) return -1;
            uint64_t v = (lh0 >> 4) | ((uint64_t)blk[pos] << 4) | ((uint64_t)blk[pos + 1] << 12) | ((uint64_t)blk[pos + 2] << 20) | ((uint64_t)blk[pos + 3] << 28);
            pos += 4;
            reg_size = (size_t)(v & 0x3FFFF);
            comp_size = (size_t)((v >> 18) & 0x3FFFF);
        }
    }

    if (reg_size > MAX_BLOCK_SIZE) return -1;
    uint8_t *lits = (uint8_t *)malloc(reg_size ? reg_size : 1);
    if (!lits) return -1;

    if (ltype == 0) {
        if (pos + reg_size > blk_len) { free(lits); return -1; }
        memcpy(lits, blk + pos, reg_size);
        pos += reg_size;
    } else if (ltype == 1) {
        if (pos >= blk_len) { free(lits); return -1; }
        memset(lits, blk[pos++], reg_size);
    } else {
        if (pos + comp_size > blk_len) { free(lits); return -1; }
        const uint8_t *lsrc = blk + pos;
        size_t lused = 0;
        if (ltype == 2) {
            uint8_t weights[256];
            unsigned n_syms = 0;
            if (huf_read_weights(lsrc, comp_size, weights, &n_syms, &lused) < 0) { free(lits); return -1; }
            if (huf_build(&c->huf, weights, n_syms) < 0) { free(lits); return -1; }
            c->has_huf = 1;
        } else {
            if (!c->has_huf) { free(lits); return -1; }
        }
        int ok = (size_fmt == 0)
            ? huf_decode_1stream(&c->huf, lsrc + lused, comp_size - lused, lits, reg_size)
            : huf_decode_4stream(&c->huf, lsrc + lused, comp_size - lused, lits, reg_size);
        if (ok < 0) { free(lits); return -1; }
        pos += comp_size;
    }

    // Sequences section
    if (pos >= blk_len) { free(lits); return -1; }
    uint8_t n0 = blk[pos++];
    size_t num_seq = 0;
    if (n0 == 0) {
        if (win_emit(c, lits, reg_size) < 0) { free(lits); return -1; }
        free(lits);
        return 0;
    } else if (n0 < 128) {
        num_seq = n0;
    } else if (n0 < 255) {
        if (pos >= blk_len) { free(lits); return -1; }
        num_seq = ((size_t)(n0 - 128) << 8) + blk[pos++];
    } else {
        if (pos + 1 >= blk_len) { free(lits); return -1; }
        num_seq = rd16(blk + pos) + 0x7F00u;
        pos += 2;
    }

    if (pos >= blk_len) { free(lits); return -1; }
    uint8_t modes = blk[pos++];
    int ll_mode = (modes >> 6) & 3;
    int of_mode = (modes >> 4) & 3;
    int ml_mode = (modes >> 2) & 3;

    int16_t norm[256];
    unsigned max_sym = 0;
    int tlog = 0;
    size_t consumed = 0;

    if (ll_mode == 0) { if (fse_build_predefined_ll(&c->ll_tab) < 0) { free(lits); return -1; } c->has_ll = 1; }
    else if (ll_mode == 1) { if (pos >= blk_len || fse_build_rle(&c->ll_tab, blk[pos++]) < 0) { free(lits); return -1; } c->has_ll = 1; }
    else if (ll_mode == 2) {
        if (fse_read_ncount(blk + pos, blk_len - pos, norm, &max_sym, &tlog, &consumed) < 0 || max_sym > 35 || tlog > 9) { free(lits); return -1; }
        pos += consumed;
        if (fse_build(&c->ll_tab, norm, max_sym, tlog) < 0) { free(lits); return -1; }
        c->has_ll = 1;
    } else if (!c->has_ll) { free(lits); return -1; }

    if (of_mode == 0) { if (fse_build_predefined_of(&c->of_tab) < 0) { free(lits); return -1; } c->has_of = 1; }
    else if (of_mode == 1) { if (pos >= blk_len || fse_build_rle(&c->of_tab, blk[pos++]) < 0) { free(lits); return -1; } c->has_of = 1; }
    else if (of_mode == 2) {
        if (fse_read_ncount(blk + pos, blk_len - pos, norm, &max_sym, &tlog, &consumed) < 0 || max_sym > 31 || tlog > 8) { free(lits); return -1; }
        pos += consumed;
        if (fse_build(&c->of_tab, norm, max_sym, tlog) < 0) { free(lits); return -1; }
        c->has_of = 1;
    } else if (!c->has_of) { free(lits); return -1; }

    if (ml_mode == 0) { if (fse_build_predefined_ml(&c->ml_tab) < 0) { free(lits); return -1; } c->has_ml = 1; }
    else if (ml_mode == 1) { if (pos >= blk_len || fse_build_rle(&c->ml_tab, blk[pos++]) < 0) { free(lits); return -1; } c->has_ml = 1; }
    else if (ml_mode == 2) {
        if (fse_read_ncount(blk + pos, blk_len - pos, norm, &max_sym, &tlog, &consumed) < 0 || max_sym > 52 || tlog > 9) { free(lits); return -1; }
        pos += consumed;
        if (fse_build(&c->ml_tab, norm, max_sym, tlog) < 0) { free(lits); return -1; }
        c->has_ml = 1;
    } else if (!c->has_ml) { free(lits); return -1; }

    bw_reader br;
    if (bw_init(&br, blk + pos, blk_len - pos) < 0) { free(lits); return -1; }
    uint16_t ll_state = (uint16_t)bw_read(&br, c->ll_tab.table_log);
    uint16_t of_state = (uint16_t)bw_read(&br, c->of_tab.table_log);
    uint16_t ml_state = (uint16_t)bw_read(&br, c->ml_tab.table_log);

    static const uint32_t ll_base[36] = {
        0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,
        16,18,20,22,24,28,32,40,48,64,128,256,512,1024,2048,4096,8192,16384,32768,65536
    };
    static const uint8_t ll_bits[36] = {
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        1,1,1,1,2,2,3,3,4,6,7,8,9,10,11,12,13,14,15,16
    };
    static const uint32_t ml_base[53] = {
        3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,
        35,37,39,41,43,47,51,59,67,83,99,131,259,515,1027,2051,4099,8195,16387,32771,65539
    };
    static const uint8_t ml_bits[53] = {
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        1,1,1,1,2,2,3,3,4,4,5,7,8,9,10,11,12,13,14,15,16
    };

    size_t lit_pos = 0;
    for (size_t i = 0; i < num_seq; i++) {
        uint8_t of_code = c->of_tab.table[of_state].symbol;
        uint8_t ml_code = c->ml_tab.table[ml_state].symbol;
        uint8_t ll_code = c->ll_tab.table[ll_state].symbol;
        if (of_code > 31 || ml_code > 52 || ll_code > 35) { free(lits); return -1; }

        uint32_t of_extra = bw_read(&br, of_code);
        uint32_t ml_extra = bw_read(&br, ml_bits[ml_code]);
        uint32_t ll_extra = bw_read(&br, ll_bits[ll_code]);

        if (i + 1 < num_seq) {
            fse_decode_sym(&c->ll_tab, &ll_state, &br);
            fse_decode_sym(&c->ml_tab, &ml_state, &br);
            fse_decode_sym(&c->of_tab, &of_state, &br);
        }

        size_t lit_len = ll_base[ll_code] + ll_extra;
        size_t match_len = ml_base[ml_code] + ml_extra;
        uint32_t raw_of = (1u << of_code) + of_extra;
        uint32_t offset = 0;
        if (raw_of > 3) {
            offset = raw_of - 3;
            c->rep[2] = c->rep[1];
            c->rep[1] = c->rep[0];
            c->rep[0] = offset;
        } else {
            uint32_t idx = raw_of - 1;
            if (lit_len == 0) idx++;
            if (idx < 3) {
                offset = c->rep[idx];
            } else {
                offset = c->rep[0] - 1;
            }
            if (idx == 1) { uint32_t t = c->rep[1]; c->rep[1] = c->rep[0]; c->rep[0] = t; }
            else if (idx == 2) { uint32_t t = c->rep[2]; c->rep[2] = c->rep[1]; c->rep[1] = c->rep[0]; c->rep[0] = t; }
            else if (idx == 3) { c->rep[2] = c->rep[1]; c->rep[1] = c->rep[0]; c->rep[0] = offset; }
        }

        if (lit_pos + lit_len > reg_size) { free(lits); return -1; }
        if (win_emit(c, lits + lit_pos, lit_len) < 0) { free(lits); return -1; }
        lit_pos += lit_len;
        if (win_copy_match(c, offset, match_len) < 0) { free(lits); return -1; }
    }

    if (lit_pos < reg_size) {
        if (win_emit(c, lits + lit_pos, reg_size - lit_pos) < 0) { free(lits); return -1; }
    }
    free(lits);
    return 0;
}

int dm_zstd_decompress_file(const char *in_path, const char *out_path, char *err_buf, size_t err_cap) {
    FILE *in = fopen(in_path, "rb");
    if (!in) return set_err(err_buf, err_cap, strerror(errno));
    FILE *out = fopen(out_path, "wb");
    if (!out) { fclose(in); return set_err(err_buf, err_cap, strerror(errno)); }

    uint8_t *blk_buf = (uint8_t *)malloc(MAX_BLOCK_SIZE);
    if (!blk_buf) { fclose(in); fclose(out); return set_err(err_buf, err_cap, "out of memory"); }

    zstd_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.out = out;

    int rc = 0;
    uint8_t mbuf[4];
    while (fread(mbuf, 1, 4, in) == 4) {
        uint32_t magic = rd32(mbuf);
        if ((magic & ZSTD_SKIP_MASK) == ZSTD_SKIP_MAGIC) {
            uint8_t sbuf[4];
            if (fread(sbuf, 1, 4, in) != 4) { rc = set_err(err_buf, err_cap, "truncated skippable frame"); break; }
            if (fseeko(in, (off_t)rd32(sbuf), SEEK_CUR) != 0) { rc = set_err(err_buf, err_cap, "seek failed"); break; }
            continue;
        }
        if (magic != ZSTD_MAGIC) { rc = set_err(err_buf, err_cap, "invalid zstd magic"); break; }

        int fhd = fgetc(in);
        if (fhd == EOF) { rc = set_err(err_buf, err_cap, "truncated frame header"); break; }
        uint8_t fcs_flag = (uint8_t)((fhd >> 6) & 3);
        uint8_t single_seg = (uint8_t)((fhd >> 5) & 1);
        uint8_t checksum_flag = (uint8_t)((fhd >> 2) & 1);
        uint8_t did_flag = (uint8_t)(fhd & 3);

        uint64_t win_size = 0;
        if (!single_seg) {
            int wd = fgetc(in);
            if (wd == EOF) { rc = set_err(err_buf, err_cap, "truncated window descriptor"); break; }
            int exp = (wd >> 3) & 31;
            int mant = wd & 7;
            uint64_t base = 1ull << (10 + exp);
            win_size = base + (base >> 3) * (uint64_t)mant;
        }

        static const int did_sizes[4] = {0, 1, 2, 4};
        if (did_sizes[did_flag] > 0) {
            uint8_t tmp[4];
            if (fread(tmp, 1, (size_t)did_sizes[did_flag], in) != (size_t)did_sizes[did_flag]) {
                rc = set_err(err_buf, err_cap, "truncated dictionary id"); break;
            }
        }

        uint64_t fcs = 0;
        static const int fcs_sizes[4] = {0, 2, 4, 8};
        int fcs_bytes = fcs_sizes[fcs_flag];
        if (fcs_flag == 0 && single_seg) fcs_bytes = 1;
        if (fcs_bytes > 0) {
            uint8_t fbuf[8] = {0};
            if (fread(fbuf, 1, (size_t)fcs_bytes, in) != (size_t)fcs_bytes) {
                rc = set_err(err_buf, err_cap, "truncated frame content size"); break;
            }
            if (fcs_bytes == 1) fcs = fbuf[0];
            else if (fcs_bytes == 2) fcs = (uint64_t)rd16(fbuf) + 256ull;
            else if (fcs_bytes == 4) fcs = rd32(fbuf);
            else fcs = rd64(fbuf);
        }
        if (single_seg) win_size = fcs ? fcs : 1;
        if (win_size > (256ull * 1024ull * 1024ull)) win_size = 256ull * 1024ull * 1024ull;

        free(ctx.win);
        ctx.win = (uint8_t *)calloc(1, (size_t)win_size);
        if (!ctx.win) { rc = set_err(err_buf, err_cap, "out of memory allocating window"); break; }
        ctx.win_size = (size_t)win_size;
        ctx.win_pos = 0;
        ctx.total_out = 0;
        ctx.has_huf = ctx.has_ll = ctx.has_of = ctx.has_ml = 0;
        ctx.rep[0] = 1; ctx.rep[1] = 4; ctx.rep[2] = 8;

        int last_blk = 0;
        while (!last_blk) {
            uint8_t bh[3];
            if (fread(bh, 1, 3, in) != 3) { rc = set_err(err_buf, err_cap, "truncated block header"); break; }
            uint32_t v = (uint32_t)bh[0] | ((uint32_t)bh[1] << 8) | ((uint32_t)bh[2] << 16);
            last_blk = (int)(v & 1u);
            uint32_t btype = (v >> 1) & 3u;
            uint32_t bsize = v >> 3;
            if (btype == 3 || bsize > MAX_BLOCK_SIZE) { rc = set_err(err_buf, err_cap, "invalid block header"); break; }
            if (btype == 0) {
                if (fread(blk_buf, 1, bsize, in) != bsize) { rc = set_err(err_buf, err_cap, "truncated raw block"); break; }
                if (win_emit(&ctx, blk_buf, bsize) < 0) { rc = set_err(err_buf, err_cap, "write failed"); break; }
            } else if (btype == 1) {
                int ch = fgetc(in);
                if (ch == EOF) { rc = set_err(err_buf, err_cap, "truncated RLE block"); break; }
                if (win_emit_byte(&ctx, (uint8_t)ch, bsize) < 0) { rc = set_err(err_buf, err_cap, "write failed"); break; }
            } else {
                if (fread(blk_buf, 1, bsize, in) != bsize) { rc = set_err(err_buf, err_cap, "truncated compressed block"); break; }
                if (decode_compressed_block(&ctx, blk_buf, bsize) < 0) { rc = set_err(err_buf, err_cap, "corrupt compressed block"); break; }
            }
        }
        if (rc != 0) break;
        if (checksum_flag) {
            uint8_t cbuf[4];
            if (fread(cbuf, 1, 4, in) != 4) { rc = set_err(err_buf, err_cap, "truncated checksum"); break; }
        }
    }

    if (rc == 0 && ferror(in)) rc = set_err(err_buf, err_cap, "read error");
    free(blk_buf);
    free(ctx.win);
    free(ctx.ll_tab.table);
    free(ctx.of_tab.table);
    free(ctx.ml_tab.table);
    free(ctx.huf.table);
    fclose(in);
    if (fclose(out) != 0 && rc == 0) rc = set_err(err_buf, err_cap, "close failed");
    return rc;
}
