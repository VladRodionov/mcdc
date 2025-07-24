#ifndef ZSTD_COMPRESSION_H
#define ZSTD_COMPRESSION_H

#include <zstd.h>

// Structure to hold the ZSTD dictionary and related data
typedef struct {
    ZSTD_CDict* cdict;
    ZSTD_DDict* ddict;
    char* dict_buffer;
    size_t dict_size;
} zstd_dict_t;

// Function to initialize the ZSTD dictionary
int zstd_init(zstd_dict_t* zstd_dict, const char* dict_path);

// Function to destroy the ZSTD dictionary
void zstd_destroy(zstd_dict_t* zstd_dict);

// Function to compress data using the ZSTD dictionary
size_t zstd_compress_item(zstd_dict_t* zstd_dict, const void* src, size_t src_size, void* dst, size_t dst_capacity);

// Function to decompress data using the ZSTD dictionary
size_t zstd_decompress_item(zstd_dict_t* zstd_dict, const void* src, size_t src_size, void* dst, size_t dst_capacity);

#endif /* ZSTD_COMPRESSION_H */
