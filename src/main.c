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

static const codec codecs[] = {
    {.name = "v0", .compress = compress_v0},
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

    printf("%-22s %7s %8s %7s %3s\n", "file", "orig", "v0", "ratio", "B");

    uint32_t total_orig = 0;
    uint32_t total_packed = 0;
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
        // would be meaningless, so every file is round-tripped here.
        v0_compress_result c = v0_compress(f.contents.view, &arena, scratch);
        v0_decompress_result d = v0_decompress(c.data.view, &arena);
        if (!d.ok || d.data.num != f.contents.num
            || memcmp(d.data.data, f.contents.data, f.contents.num) != 0) {
            fprintf(stderr, "bench: %s failed round trip\n", path);
            rc = 1;
            continue;
        }

        printf("%-22s %7u %8u %6.1f%% %3u\n", corpus_files[k], f.contents.num,
               c.data.num, 100.0 * c.data.num / f.contents.num, c.b);
        total_orig += f.contents.num;
        total_packed += c.data.num;
        rc_arena_free_to(&arena, mark);
    }

    if (total_orig > 0) {
        printf("%-22s %7u %8u %6.1f%%\n", "TOTAL", total_orig, total_packed,
               100.0 * total_packed / total_orig);
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
