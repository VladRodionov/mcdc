#include "zstd_compression.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int zstd_init(zstd_dict_t* zstd_dict, const char* dict_path) {
    FILE* const f = fopen(dict_path, "rb");
    if (f==NULL) {
        // Dictionary file not found.
        zstd_dict->dict_buffer = NULL;
        zstd_dict->dict_size = 0;
        zstd_dict->cdict = NULL;
        zstd_dict->ddict = NULL;
        return 0;
    }

    fseek(f, 0, SEEK_END);
    long const file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (file_size <= 0) {
        fclose(f);
        return 1; // Error
    }

    zstd_dict->dict_size = file_size;
    zstd_dict->dict_buffer = malloc(zstd_dict->dict_size);
    if (!zstd_dict->dict_buffer) {
        fclose(f);
        return 1; // Error
    }

    size_t const read_size = fread(zstd_dict->dict_buffer, 1, zstd_dict->dict_size, f);
    fclose(f);
    if (read_size != zstd_dict->dict_size) {
        free(zstd_dict->dict_buffer);
        zstd_dict->dict_buffer = NULL;
        return 1; // Error
    }

    zstd_dict->cdict = ZSTD_createCDict(zstd_dict->dict_buffer, zstd_dict->dict_size, 1);
    zstd_dict->ddict = ZSTD_createDDict(zstd_dict->dict_buffer, zstd_dict->dict_size);

    if (!zstd_dict->cdict || !zstd_dict->ddict) {
        zstd_destroy(zstd_dict);
        return 1; // Error
    }

    return 0;
}

void zstd_destroy(zstd_dict_t* zstd_dict) {
    if (zstd_dict->cdict) ZSTD_freeCDict(zstd_dict->cdict);
    if (zstd_dict->ddict) ZSTD_freeDDict(zstd_dict->ddict);
    if (zstd_dict->dict_buffer) free(zstd_dict->dict_buffer);
}

size_t zstd_compress_item(zstd_dict_t* zstd_dict, const void* src, size_t src_size, void* dst, size_t dst_capacity) {
    if (zstd_dict->cdict) {
        ZSTD_CCtx* const cctx = ZSTD_createCCtx();
        size_t const cSize = ZSTD_compress_usingCDict(cctx, dst, dst_capacity, src, src_size, zstd_dict->cdict);
        ZSTD_freeCCtx(cctx);
        return cSize;
    } else {
        return ZSTD_compress(dst, dst_capacity, src, src_size, 1);
    }
}

size_t zstd_decompress_item(zstd_dict_t* zstd_dict, const void* src, size_t src_size, void* dst, size_t dst_capacity) {
    if (zstd_dict->ddict) {
        ZSTD_DCtx* const dctx = ZSTD_createDCtx();
        size_t const rSize = ZSTD_decompress_usingDDict(dctx, dst, dst_capacity, src, src_size, zstd_dict->ddict);
        ZSTD_freeDCtx(dctx);
        return rSize;
    } else {
        return ZSTD_decompress(dst, dst_capacity, src, src_size);
    }
}

