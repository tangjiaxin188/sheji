#ifndef EASY_MINIZ_H
#define EASY_MINIZ_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 压缩结果状态码
 */
typedef enum {
    EASY_MINIZ_OK = 0,           /* 成功 */
    EASY_MINIZ_ERR_MEMORY = -1,  /* 内存分配失败 */
    EASY_MINIZ_ERR_PARAM = -2,   /* 参数错误 */
    EASY_MINIZ_ERR_COMPRESS = -3,/* 压缩失败 */
    EASY_MINIZ_ERR_DECOMPRESS = -4 /* 解压缩失败 */
} easy_miniz_status;

/*
 * 压缩数据块结构
 * 包含压缩后的数据和大小，以及解压后的大小（用于解压缩时分配内存）
 */
typedef struct {
    void *data;           /* 数据指针 */
    size_t compressed_size;   /* 压缩后大小 */
    size_t uncompressed_size; /* 解压后大小 */
} easy_miniz_block;

/*
 * 初始化 easy_miniz 库
 * 返回 0 表示成功，非 0 表示失败
 */
int easy_miniz_init(void);

/*
 * 清理 easy_miniz 库
 * 释放所有内部资源
 */
void easy_miniz_uninit(void);

/*
 * 压缩内存数据
 * 
 * 参数:
 *   src      - 源数据指针
 *   src_size - 源数据大小
 *   level    - 压缩级别 (0-9, 0 无压缩，9 最佳压缩，-1 默认)
 * 
 * 返回:
 *   成功返回 easy_miniz_block 指针，失败返回 NULL
 * 
 * 注意:
 *   返回的 block 必须使用 easy_miniz_free_block() 释放
 */
easy_miniz_block *easy_miniz_compress(const void *src, size_t src_size, int level);

/*
 * 解压缩内存数据
 * 
 * 参数:
 *   block - 压缩数据块（由 easy_miniz_compress 返回）
 * 
 * 返回:
 *   成功返回解压后的数据指针，失败返回 NULL
 * 
 * 注意:
 *   返回的数据必须使用 easy_miniz_free() 释放
 */
void *easy_miniz_decompress(const easy_miniz_block *block);

/*
 * 释放压缩数据块
 * 
 * 参数:
 *   block - 要释放的压缩数据块
 */
void easy_miniz_free_block(easy_miniz_block *block);

/*
 * 释放内存
 * 
 * 参数:
 *   ptr - 要释放的内存指针
 */
void easy_miniz_free(void *ptr);

/*
 * 获取最后一个错误的描述信息
 */
const char *easy_miniz_error_string(int status);

#ifdef __cplusplus
}
#endif

#endif /* EASY_MINIZ_H */
