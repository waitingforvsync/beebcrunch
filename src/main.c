/*
 *  main.c - beebcrunch command line
 *
 *  beebcrunch <input> <output> -c <codec>   compress a file
 *  beebcrunch bench [corpus_dir]            corpus benchmark (default: corpus)
 *  beebcrunch --test [prefix]               run unit tests (ENABLE_TESTS builds)
 */

#include <stdio.h>
#include <string.h>

#include "richc/arena.h"
#include "richc/file.h"

#include "v0.h"
#include "v1.h"
#include "v2.h"
#include "v3.h"
#include "v4.h"
#include "v5.h"

#ifdef ENABLE_TESTS
#include "richc/test.h"
#endif

typedef struct codec {
    const char *name;
    rc_array_bytes (*compress)(rc_view_bytes in, rc_arena *arena, rc_arena scratch);
} codec;

static rc_array_bytes compress_v0(rc_view_bytes in, rc_arena *arena, rc_arena scratch)
{
    return v0_compress(in, arena, scratch).data;
}

// A codec that cannot represent the input returns an empty array (a valid
// stream is never smaller than the 2-byte header).
static rc_array_bytes compress_v1(rc_view_bytes in, rc_arena *arena, rc_arena scratch)
{
    v1_compress_result c = v1_compress(in, arena, scratch);
    return c.ok ? c.data : (rc_array_bytes) {0};
}

static rc_array_bytes compress_v2(rc_view_bytes in, rc_arena *arena, rc_arena scratch)
{
    return v2_compress(in, arena, scratch).data;
}

static rc_array_bytes compress_v3(rc_view_bytes in, rc_arena *arena, rc_arena scratch)
{
    v3_compress_result c = v3_compress(in, arena, scratch);
    return c.ok ? c.data : (rc_array_bytes) {0};
}

static rc_array_bytes compress_v4(rc_view_bytes in, rc_arena *arena, rc_arena scratch)
{
    return v4_compress(in, arena, scratch).data;
}

static rc_array_bytes compress_v5(rc_view_bytes in, rc_arena *arena, rc_arena scratch)
{
    v5_compress_result c = v5_compress(in, arena, scratch);
    return c.ok ? c.data : (rc_array_bytes) {0};
}

static const codec codecs[] = {
    {.name = "v0", .compress = compress_v0},
    {.name = "v1", .compress = compress_v1},
    {.name = "v2", .compress = compress_v2},
    {.name = "v3", .compress = compress_v3},
    {.name = "v4", .compress = compress_v4},
    {.name = "v5", .compress = compress_v5},
};

static const char *corpus_files[] = {
    "exile-title.bin",
    "droid-title.bin",
    "ravenskull-title.bin",
    "repton3-title.bin",
    "boomscreen.bin",
    "blurpscreen.bin",
    "exileb.bin",
    "chuckie.bin",
    "frak2.bin",
    "blurp.bin",
    "basic2.rom",
};

static int usage(void)
{
    fprintf(stderr,
            "usage: beebcrunch <input> <output> -c <codec>\n"
            "       beebcrunch bench [corpus_dir]\n"
            "       beebcrunch --test [prefix]\n"
            "codecs:");
    for (uint32_t k = 0; k < sizeof codecs / sizeof codecs[0]; k++) {
        fprintf(stderr, " %s", codecs[k].name);
    }
    fprintf(stderr, "\n");
    return 2;
}

static const codec *find_codec(const char *name)
{
    for (uint32_t k = 0; k < sizeof codecs / sizeof codecs[0]; k++) {
        if (strcmp(codecs[k].name, name) == 0) {
            return &codecs[k];
        }
    }
    return NULL;
}

static int cmd_compress(const char *infile, const char *outfile, const char *codec_name)
{
    const codec *c = find_codec(codec_name);
    if (!c) {
        fprintf(stderr, "beebcrunch: unknown codec '%s'\n", codec_name);
        return usage();
    }

    rc_arena arena = rc_arena_make_default();
    rc_arena scratch = rc_arena_make_default();

    rc_file_load_binary_result f =
        rc_file_load_binary(rc_str_from_cstr(infile), 0, &arena);
    if (f.error != RC_FILE_OK) {
        fprintf(stderr, "beebcrunch: cannot read %s\n", infile);
        return 1;
    }
    if (f.contents.num > v0_max_uncompressed) {
        fprintf(stderr, "beebcrunch: %s is %u bytes; the maximum is %u\n",
                infile, f.contents.num, (uint32_t)v0_max_uncompressed);
        return 1;
    }

    rc_array_bytes packed = c->compress(f.contents.view, &arena, scratch);
    if (packed.num == 0) {
        fprintf(stderr, "beebcrunch: %s cannot represent %s\n", c->name, infile);
        return 1;
    }
    if (rc_file_save_binary(rc_str_from_cstr(outfile), packed.view) != RC_FILE_OK) {
        fprintf(stderr, "beebcrunch: cannot write %s\n", outfile);
        return 1;
    }
    printf("%s: %u -> %u bytes (%.1f%%) [%s]\n", infile, f.contents.num, packed.num,
           f.contents.num ? 100.0 * packed.num / f.contents.num : 0.0, c->name);

    rc_arena_deinit(&scratch);
    rc_arena_deinit(&arena);
    return 0;
}

static int cmd_bench(const char *corpus_dir)
{
    rc_arena arena = rc_arena_make_default();
    rc_arena scratch = rc_arena_make_default();
    int rc = 0;

    printf("%-22s %7s %8s %8s %8s %8s %8s %8s %7s\n", "file", "orig", "v0", "v1", "v2", "v3", "v4", "v5", "ratio");

    uint32_t total_orig = 0;
    uint32_t total_v0 = 0;
    uint32_t total_v1 = 0;
    uint32_t total_v2 = 0;
    uint32_t total_v3 = 0;
    uint32_t total_v4 = 0;
    uint32_t total_v5 = 0;
    bool v1_all_ok = true;
    for (uint32_t k = 0; k < sizeof corpus_files / sizeof corpus_files[0]; k++) {
        char path[512];
        snprintf(path, sizeof path, "%s/%s", corpus_dir, corpus_files[k]);

        uint32_t mark = arena.top;
        rc_file_load_binary_result f =
            rc_file_load_binary(rc_str_from_cstr(path), 0, &arena);
        if (f.error != RC_FILE_OK) {
            fprintf(stderr, "bench: cannot load %s\n", path);
            rc = 1;
            continue;
        }

        // Numbers for a stream that does not decode back to the original
        // would be meaningless, so every codec is round-tripped here.
        v0_compress_result c0 = v0_compress(f.contents.view, &arena, scratch);
        v0_decompress_result d0 = v0_decompress(c0.data.view, &arena);
        if (!d0.ok || d0.data.num != f.contents.num
            || memcmp(d0.data.data, f.contents.data, f.contents.num) != 0) {
            fprintf(stderr, "bench: %s failed v0 round trip\n", path);
            rc = 1;
            continue;
        }

        v1_compress_result c1 = v1_compress(f.contents.view, &arena, scratch);
        uint32_t v1_size = 0;
        if (c1.ok) {
            v1_decompress_result d1 = v1_decompress(c1.data.view, &arena);
            if (!d1.ok || d1.data.num != f.contents.num
                || memcmp(d1.data.data, f.contents.data, f.contents.num) != 0) {
                fprintf(stderr, "bench: %s failed v1 round trip\n", path);
                rc = 1;
                continue;
            }
            v1_size = c1.data.num;
            total_v1 += c1.data.num;
        }
        else {
            v1_all_ok = false;
        }

        v2_compress_result c2 = v2_compress(f.contents.view, &arena, scratch);
        v2_decompress_result d2 = v2_decompress(c2.data.view, &arena);
        if (!d2.ok || d2.data.num != f.contents.num
            || memcmp(d2.data.data, f.contents.data, f.contents.num) != 0) {
            fprintf(stderr, "bench: %s failed v2 round trip\n", path);
            rc = 1;
            continue;
        }

        v3_compress_result c3 = v3_compress(f.contents.view, &arena, scratch);
        uint32_t v3_size = 0;
        bool v3_ok = c3.ok;
        if (c3.ok) {
            v3_decompress_result d3 = v3_decompress(c3.data.view, &arena);
            if (!d3.ok || d3.data.num != f.contents.num
                || memcmp(d3.data.data, f.contents.data, f.contents.num) != 0) {
                fprintf(stderr, "bench: %s failed v3 round trip\n", path);
                rc = 1;
                continue;
            }
            v3_size = c3.data.num;
            total_v3 += c3.data.num;
        }

        v4_compress_result c4 = v4_compress(f.contents.view, &arena, scratch);
        v4_decompress_result d4 = v4_decompress(c4.data.view, &arena);
        if (!d4.ok || d4.data.num != f.contents.num
            || memcmp(d4.data.data, f.contents.data, f.contents.num) != 0) {
            fprintf(stderr, "bench: %s failed v4 round trip\n", path);
            rc = 1;
            continue;
        }

        v5_compress_result c5 = v5_compress(f.contents.view, &arena, scratch);
        uint32_t v5_size = 0;
        bool v5_ok = c5.ok;
        if (c5.ok) {
            v5_decompress_result d5 = v5_decompress(c5.data.view, &arena);
            if (!d5.ok || d5.data.num != f.contents.num
                || memcmp(d5.data.data, f.contents.data, f.contents.num) != 0) {
                fprintf(stderr, "bench: %s failed v5 round trip\n", path);
                rc = 1;
                continue;
            }
            v5_size = c5.data.num;
            total_v5 += c5.data.num;
        }

        char v1_col[16];
        char v3_col[16];
        char v5_col[16];
        if (c1.ok) {
            snprintf(v1_col, sizeof v1_col, "%u", v1_size);
        }
        else {
            snprintf(v1_col, sizeof v1_col, "-");
        }
        if (v3_ok) {
            snprintf(v3_col, sizeof v3_col, "%u", v3_size);
        }
        else {
            snprintf(v3_col, sizeof v3_col, "-");
        }
        if (v5_ok) {
            snprintf(v5_col, sizeof v5_col, "%u", v5_size);
        }
        else {
            snprintf(v5_col, sizeof v5_col, "-");
        }
        printf("%-22s %7u %8u %8s %8u %8s %8u %8s %6.1f%%\n", corpus_files[k],
               f.contents.num, c0.data.num, v1_col, c2.data.num, v3_col,
               c4.data.num, v5_col,
               v5_ok ? 100.0 * v5_size / f.contents.num
                     : 100.0 * c4.data.num / f.contents.num);
        total_orig += f.contents.num;
        total_v0 += c0.data.num;
        total_v2 += c2.data.num;
        total_v4 += c4.data.num;
        rc_arena_free_to(&arena, mark);
    }

    if (total_orig > 0) {
        printf("%-22s %7u %8u %8u %8u %8u %8u %8u %6.1f%%%s\n", "TOTAL", total_orig, total_v0,
               total_v1, total_v2, total_v3, total_v4, total_v5,
               100.0 * total_v5 / total_orig,
               v1_all_ok ? "" : " (v1 total excludes failed files)");
    }

    rc_arena_deinit(&scratch);
    rc_arena_deinit(&arena);
    return rc;
}

static int cmd_test(const char *prefix)
{
#ifdef ENABLE_TESTS
    return rc_test_run(prefix);
#else
    (void)prefix;
    fprintf(stderr, "beebcrunch: built without ENABLE_TESTS\n");
    return 1;
#endif
}

int main(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "--test") == 0) {
        if (argc > 3) {
            return usage();
        }
        return cmd_test(argc == 3 ? argv[2] : "");
    }
    if (argc >= 2 && strcmp(argv[1], "bench") == 0) {
        if (argc > 3) {
            return usage();
        }
        return cmd_bench(argc == 3 ? argv[2] : "corpus");
    }
    if (argc == 5 && strcmp(argv[3], "-c") == 0) {
        return cmd_compress(argv[1], argv[2], argv[4]);
    }
    return usage();
}
