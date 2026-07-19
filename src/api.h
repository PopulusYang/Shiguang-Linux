#pragma once

/* GBytes 前向声明，避免 api.h 引入 glib.h */
typedef struct _GBytes GBytes;

/**
 * 图片数据
 */
typedef struct {
    char *title;       /* 标题 */
    char *imgurl;      /* 原图链接 */
    char *thumburl;    /* 缩略图链接 */
    char *copyright;   /* 版权信息 */
    char *reldate;     /* 日期 */
    char *story;       /* 图片故事/描述 */
    int   width;       /* 宽度 */
    int   height;      /* 高度 */
} ImageData;

/**
 * 图源列表
 */
extern const char *PROVIDERS[];
extern const int   PROVIDER_COUNT;

/**
 * 获取今日一图
 * @param provider 图源 ID（如 "timeline", "bing"）
 * @return ImageData 指针，调用方负责释放（api_image_free）
 */
ImageData *api_today(const char *provider);

/**
 * 获取随机一图
 * @param provider 图源 ID
 * @return ImageData 指针，调用方负责释放
 */
ImageData *api_random(const char *provider);

/**
 * 下载图片到内存
 * @param url  图片 URL
 * @param len  输出：数据长度
 * @return 图片字节数据，调用方负责 g_free
 */
GBytes *api_download_image(const char *url);

/**
 * 下载图片到文件
 * @param url      图片 URL
 * @param filepath 目标文件路径
 * @return TRUE 成功
 */
int api_download_to_file(const char *url, const char *filepath);

/* ---- 收藏功能 ---- */

/** 检查图片是否已收藏（标题为空时从 URL 提取文件名）*/
int api_is_favorited(const char *title, const char *url);

/** 下载图片到收藏目录（自动处理重名：追加 (1)(2) 等） */
int api_save_favorite(const char *url, const char *title);

/** 收藏数量 */
int api_favorites_count(void);

/** 按索引获取收藏图片的完整路径（调用方 g_free） */
char *api_favorite_path_by_index(int index);

/**
 * 释放 ImageData
 */
void api_image_free(ImageData *img);
