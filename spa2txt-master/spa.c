#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <errno.h>
#include "nicolet_spa.h"

typedef union {
    uint8_t u8[2];
    uint16_t u16;
} endianness_test;

static int host_is_big_endian(void) {
    endianness_test t;
    t.u16 = 0x0100;
    return t.u8[0] != 0;
}

static uint16_t le16(const uint8_t* p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}
static uint32_t le32(const uint8_t* p) {
    return (uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24));
}
static float le_f32(const uint8_t* p) {
    uint32_t u = le32(p);
    float f;
    memcpy(&f, &u, sizeof(float));
    return f;
}

// byte swapping will occur for each `size`-sized item (only matters on big-endian)
static size_t fread_le(void* ptr, size_t size, size_t nmemb, FILE* stream) {
    size_t ret = fread(ptr, size, nmemb, stream);
    if (ret != nmemb) return ret;

    if (host_is_big_endian()) {
        for (size_t i = 0; i < nmemb; i++) {
            uint8_t* p = (uint8_t*)ptr + i * size;
            for (size_t j = 0; j < size / 2; j++) {
                uint8_t tmp = p[j];
                p[j] = p[size - 1 - j];
                p[size - 1 - j] = tmp;
            }
        }
    }
    return nmemb;
}

static long file_size_bytes(FILE* fh) {
    if (fseek(fh, 0, SEEK_END) != 0) return -1;
    long sz = ftell(fh);
    if (sz < 0) return -1;
    if (fseek(fh, 0, SEEK_SET) != 0) return -1;
    return sz;
}

typedef struct {
    uint16_t type;     // 2 bytes
    uint32_t offset;   // 4 bytes (starts at +2)
    uint32_t size;     // 4 bytes (starts at +6)
    uint16_t flags;    // 2 bytes (starts at +10)
    uint32_t reserved; // 4 bytes (starts at +12)
} spa_dir_entry;

// read SPA block directory used by your OMNIC-exported SPA (16 bytes/entry), starting at 0x130
static int read_dir(FILE* fh, long fsz, spa_dir_entry* out, size_t out_cap, size_t* out_n) {
    *out_n = 0;
    if (fseek(fh, 0x130, SEEK_SET) != 0) return 0;

    uint8_t buf[16];
    int zero_run = 0;

    for (size_t i = 0; i < out_cap; i++) {
        if (fread(buf, 1, 16, fh) != 16) break;

        spa_dir_entry e;
        e.type = le16(buf + 0);
        e.offset = le32(buf + 2);
        e.size = le32(buf + 6);
        e.flags = le16(buf + 10);
        e.reserved = le32(buf + 12);

        // terminator-ish: many SPA files have lots of (type!=0 but offset=0 size=0) at the end
        if (e.offset == 0 && e.size == 0) {
            zero_run++;
            if (zero_run >= 4) break;
            continue;
        }
        zero_run = 0;

        // sanity
        if ((long)e.offset <= 0 || (long)e.size <= 0) continue;
        if ((long)e.offset >= fsz) continue;
        if ((long)e.offset + (long)e.size > fsz) continue;

        out[(*out_n)++] = e;
    }
    return *out_n > 0;
}

enum spa_parse_result spa_parse(
    const char* filename,
    char comment[256], size_t* num_points,
    float** wavelengths, float** intensities
) {
    *wavelengths = *intensities = NULL;
    *num_points = 0;

    enum spa_parse_result ret = spa_ok;
    FILE* fh = fopen(filename, "rb");
    if (!fh) return spa_open_error;

    long fsz = file_size_bytes(fh);
    if (fsz <= 0) { ret = spa_read_error; goto cleanup; }

    // comment/title: offset 30 length 255
    if (fseek(fh, 30, SEEK_SET) != 0) { ret = spa_seek_error; goto cleanup; }
    if (fread(comment, 255, 1, fh) != 1) { ret = spa_read_error; goto cleanup; }
    comment[255] = '\0';

    // -------- New OMNIC SPA variant (your files): parse directory entries and locate blocks --------
    spa_dir_entry ents[256];
    size_t nents = 0;
    long meta_off = -1, meta_size = -1;
    long data_off = -1, data_size = -1;

    if (read_dir(fh, fsz, ents, 256, &nents)) {
        for (size_t i = 0; i < nents; i++) {
            if (ents[i].type == 2 && meta_off < 0) { // axis/metadata block
                meta_off = (long)ents[i].offset;
                meta_size = (long)ents[i].size;
            }
            if (ents[i].type == 3 && data_off < 0) { // intensity float array block
                data_off = (long)ents[i].offset;
                data_size = (long)ents[i].size;
            }
        }
    }

    if (meta_off > 0 && data_off > 0 && meta_size >= 24 && data_size >= 4) {
        uint8_t* meta = (uint8_t*)malloc((size_t)meta_size);
        if (!meta) { ret = spa_alloc_error; goto cleanup; }

        if (fseek(fh, meta_off, SEEK_SET) != 0) { free(meta); ret = spa_seek_error; goto cleanup; }
        if (fread(meta, 1, (size_t)meta_size, fh) != (size_t)meta_size) { free(meta); ret = spa_read_error; goto cleanup; }

        // In your SPA: meta+4 is uint32 num_points, meta+16/meta+20 are float x_max/x_min (cm-1)
        uint32_t n = le32(meta + 4);
        float wn_max = le_f32(meta + 16);
        float wn_min = le_f32(meta + 20);

        // If header n is missing, infer from data block size
        size_t inferred = (size_t)(data_size / 4);
        if (n == 0) n = (uint32_t)inferred;
        if (n == 0) { free(meta); ret = spa_read_error; goto cleanup; }

        // guard: don’t read beyond block
        if ((size_t)n > inferred) n = (uint32_t)inferred;

        // plausibility check for wn range; if broken, still output index-based axis
        int wn_ok = (wn_max > 0.0f && wn_min >= 0.0f && wn_max <= 20000.0f && wn_min <= 20000.0f && wn_max != wn_min);

        *num_points = (size_t)n;
        *intensities = (float*)calloc(*num_points, sizeof(float));
        *wavelengths = (float*)calloc(*num_points, sizeof(float));
        if (!*intensities || !*wavelengths) { free(meta); ret = spa_alloc_error; goto cleanup; }

        if (fseek(fh, data_off, SEEK_SET) != 0) { free(meta); ret = spa_seek_error; goto cleanup; }
        if (fread_le(*intensities, sizeof(float), *num_points, fh) != *num_points) { free(meta); ret = spa_read_error; goto cleanup; }

        if (wn_ok && *num_points > 1) {
            for (size_t i = 0; i < *num_points; i++) {
                (*wavelengths)[i] = wn_max - (wn_max - wn_min) * (float)i / (float)(*num_points - 1);
            }
        }
        else {
            // fallback: still write something monotonic if wn not found
            for (size_t i = 0; i < *num_points; i++) (*wavelengths)[i] = (float)i;
        }

        free(meta);
        goto cleanup_success;
    }

    // -------- Fallback: old SPA variant (original repo logic) --------
    {
        uint32_t local_num_points = 0;
        float wn_maxmin[2] = { 0 };
        uint16_t flag = 0, offset = 0;

        if (fseek(fh, 564, SEEK_SET) != 0) { ret = spa_seek_error; goto cleanup; }
        if (fread_le(&local_num_points, sizeof(uint32_t), 1, fh) != 1) { ret = spa_read_error; goto cleanup; }
        *num_points = (size_t)local_num_points;

        if (fseek(fh, 576, SEEK_SET) != 0) { ret = spa_seek_error; goto cleanup; }
        if (fread_le(wn_maxmin, sizeof(float), 2, fh) != 2) { ret = spa_read_error; goto cleanup; }

        if (fseek(fh, 288, SEEK_SET) != 0) { ret = spa_seek_error; goto cleanup; }
        do {
            if (fread_le(&flag, sizeof(uint16_t), 1, fh) != 1) { ret = spa_read_error; goto cleanup; }
        } while (flag != 3);
        if (fread_le(&offset, sizeof(uint16_t), 1, fh) != 1) { ret = spa_read_error; goto cleanup; }

        *intensities = (float*)calloc(*num_points, sizeof(float));
        *wavelengths = (float*)calloc(*num_points, sizeof(float));
        if (!*intensities || !*wavelengths) { ret = spa_alloc_error; goto cleanup; }

        if (fseek(fh, offset, SEEK_SET) != 0) { ret = spa_seek_error; goto cleanup; }
        if (fread_le(*intensities, sizeof(float), *num_points, fh) != *num_points) { ret = spa_read_error; goto cleanup; }

        if (*num_points > 1) {
            for (size_t i = 0; i < *num_points; i++) {
                (*wavelengths)[i] = wn_maxmin[0] - (wn_maxmin[0] - wn_maxmin[1]) * (float)i / (float)(*num_points - 1);
            }
        }
        else if (*num_points == 1) {
            (*wavelengths)[0] = wn_maxmin[0];
        }
    }

cleanup_success:
    fclose(fh);
    return spa_ok;

cleanup:
    if (fh) fclose(fh);
    if (ret) {
        free(*wavelengths);
        free(*intensities);
        *wavelengths = *intensities = NULL;
        *num_points = 0;
    }
    return ret;
}