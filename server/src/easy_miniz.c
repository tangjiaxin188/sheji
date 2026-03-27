#include "easy_miniz.h"
#include "miniz.h"
#include <stdlib.h>
#include <string.h>

/* 库初始化状态 */
static int g_easy_miniz_initialized = 0;

/* 错误信息描述 */
static const char *g_easy_miniz_errors[] = {
    "Success",
    "Memory allocation failed",
    "Invalid parameter",
    "Compression failed",
    "Decompression failed"
};

int easy_miniz_init(void)
{
    if (g_easy_miniz_initialized) {
        return 0;
    }
    
    /* miniz 不需要特别的初始化，这里只是标记库已初始化 */
    g_easy_miniz_initialized = 1;
    return 0;
}

void easy_miniz_uninit(void)
{
    /* miniz 不需要特别的清理工作 */
    g_easy_miniz_initialized = 0;
}

easy_miniz_block *easy_miniz_compress(const void *src, size_t src_size, int level)
{
    easy_miniz_block *block = NULL;
    size_t compressed_size = 0;
    void *compressed_data = NULL;
    
    /* 参数检查 */
    if (!src || src_size == 0) {
        return NULL;
    }
    
    /* 检查初始化状态 */
    if (!g_easy_miniz_initialized) {
        return NULL;
    }
    
    /* 分配 block 结构 */
    block = (easy_miniz_block *)malloc(sizeof(easy_miniz_block));
    if (!block) {
        return NULL;
    }
    
    /* 计算压缩后的最大大小 */
    compressed_size = mz_compressBound(src_size);
    
    /* 分配压缩数据缓冲区 */
    compressed_data = malloc(compressed_size);
    if (!compressed_data) {
        free(block);
        return NULL;
    }
    
    /* 执行压缩 */
    {
        int status = mz_compress2((unsigned char *)compressed_data, 
                                   (mz_ulong *)&compressed_size, 
                                   (const unsigned char *)src, 
                                   (mz_ulong)src_size, 
                                   level);
        
        if (status != MZ_OK) {
            free(compressed_data);
            free(block);
            return NULL;
        }
    }
    
    /* 填充 block 结构 */
    block->data = compressed_data;
    block->compressed_size = compressed_size;
    block->uncompressed_size = src_size;
    
    return block;
}

void *easy_miniz_decompress(const easy_miniz_block *block)
{
    void *uncompressed_data = NULL;
    mz_ulong uncompressed_size = 0;
    
    /* 参数检查 */
    if (!block || !block->data || block->compressed_size == 0) {
        return NULL;
    }
    
    /* 检查初始化状态 */
    if (!g_easy_miniz_initialized) {
        return NULL;
    }
    
    /* 使用 block 中保存的解压后大小 */
    uncompressed_size = (mz_ulong)block->uncompressed_size;
    
    /* 分配解压后的缓冲区 */
    uncompressed_data = malloc((size_t)uncompressed_size);
    if (!uncompressed_data) {
        return NULL;
    }
    
    /* 执行解压缩 */
    {
        int status = mz_uncompress((unsigned char *)uncompressed_data,
                                    &uncompressed_size,
                                    (const unsigned char *)block->data,
                                    (mz_ulong)block->compressed_size);
        
        if (status != MZ_OK) {
            free(uncompressed_data);
            return NULL;
        }
    }
    
    return uncompressed_data;
}

void easy_miniz_free_block(easy_miniz_block *block)
{
    if (block) {
        if (block->data) {
            free(block->data);
            block->data = NULL;
        }
        free(block);
    }
}

void easy_miniz_free(void *ptr)
{
    if (ptr) {
        free(ptr);
    }
}

const char *easy_miniz_error_string(int status)
{
    int num_errors = sizeof(g_easy_miniz_errors) / sizeof(g_easy_miniz_errors[0]);
    int index = -status;
    
    if (index >= 0 && index < num_errors) {
        return g_easy_miniz_errors[index];
    }
    
    return "Unknown error";
}
