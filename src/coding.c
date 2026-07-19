/*
 *  coding.c - the shared coding composition
 */

#include "coding.h"

#include "richc/macros.h"

uint32_t coding_transmit_bits(const coding_tables *t, uint32_t first_ctx)
{
    uint32_t bits = table_transmit_bits(&t->len);
    for (uint32_t c = first_ctx; c < num_contexts; c++) {
        bits += table_transmit_bits(&t->off[c]);
    }
    return bits;
}

void write_coding_tables(bitwriter *w, const coding_tables *t, uint32_t first_ctx)
{
    for (uint32_t c = first_ctx; c < num_contexts; c++) {
        write_table(w, &t->off[c]);
    }
    write_table(w, &t->len);
}

coding_tables_result read_coding_tables(bitreader *r, uint32_t first_ctx,
                                        uint32_t len_minval)
{
    coding_tables_result res = {.ok = true};
    for (uint32_t c = first_ctx; c < num_contexts; c++) {
        table_result off = read_table(r, 1, off_ctx_index_bits(c), off_ctx_buckets(c));
        if (!off.ok) {
            res.ok = false;
            return res;
        }
        res.tables.off[c] = off.table;
    }
    table_result len = read_table(r, len_minval, 0, len_max_buckets);
    if (!len.ok) {
        res.ok = false;
        return res;
    }
    res.tables.len = len.table;
    return res;
}

coding_tables seed_coding_tables(uint32_t first_ctx, uint32_t len_minval)
{
    coding_tables t = {
        .len = {
            .minval = len_minval,
            .num_buckets = 9,
            .index_bits = 0,
        },
    };
    if (first_ctx == 0) {
        t.off[0] = (table) {
            .minval = 1,
            .num_buckets = 4,
            .index_bits = off_ctx_index_bits(0),
            .width = {2, 3, 4, 5},
        };
        table_build_starts(&t.off[0]);
    }
    for (uint32_t c = 1; c < num_contexts; c++) {
        t.off[c] = (table) {
            .minval = 1,
            .num_buckets = 16,
            .index_bits = off_index_bits,
        };
        for (uint32_t i = 0; i < t.off[c].num_buckets; i++) {
            t.off[c].width[i] = i;
        }
        table_build_starts(&t.off[c]);
    }
    for (uint32_t i = 0; i < t.len.num_buckets; i++) {
        t.len.width[i] = i;
    }
    table_build_starts(&t.len);
    return t;
}
