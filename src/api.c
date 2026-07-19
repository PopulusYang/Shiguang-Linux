#define G_LOG_DOMAIN "api"

#include "api.h"
#include <glib.h>
#include <curl/curl.h>
#include <json-glib/json-glib.h>
#include <string.h>

#define BASE_URL "https://api.nguaduot.cn"

/* 可用图源列表 */
const char *PROVIDERS[] = {
    "timeline",
    "glutton",
    "snake",
    "bing",
    "nasa",
    "unsplash",
};
const int PROVIDER_COUNT = G_N_ELEMENTS(PROVIDERS);

/* ---- HTTP 回调：把响应数据写入 GString ---- */
struct curl_cb_ctx {
    GString *buf;
};

static size_t curl_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
    struct curl_cb_ctx *ctx = userdata;
    size_t total = size * nmemb;
    g_string_append_len(ctx->buf, ptr, total);
    return total;
}

/* ---- 发起 GET 请求，返回响应体字符串（失败返回 NULL）---- */
static char *http_get(const char *url) {
    g_debug("HTTP GET %s", url);

    CURL *curl = curl_easy_init();
    if (!curl) {
        g_warning("curl_easy_init() 失败");
        return NULL;
    }

    GString *buf = g_string_new(NULL);
    struct curl_cb_ctx ctx = { buf };

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);

    CURLcode res = curl_easy_perform(curl);

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    double total_time = 0;
    curl_easy_getinfo(curl, CURLINFO_TOTAL_TIME, &total_time);

    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        g_warning("HTTP 请求失败: %s (%.2fs)", curl_easy_strerror(res), total_time);
        g_string_free(buf, TRUE);
        return NULL;
    }

    if (http_code != 200) {
        g_warning("HTTP %ld (%s) — 期望 200", http_code, url);
        g_string_free(buf, TRUE);
        return NULL;
    }

    g_debug("HTTP 200 — %zu 字节, 耗时 %.2fs", buf->len, total_time);
    return g_string_free(buf, FALSE); /* 返回内部 char*，调用方 g_free */
}

/* ---- 从 JSON 中提取 ImageData ---- */
static ImageData *parse_image_data(JsonReader *reader) {
    ImageData *img = g_new0(ImageData, 1);

    json_reader_read_member(reader, "title");
    img->title = g_strdup(json_reader_get_string_value(reader) ?: "");
    json_reader_end_member(reader);

    json_reader_read_member(reader, "imgurl");
    img->imgurl = g_strdup(json_reader_get_string_value(reader) ?: "");
    json_reader_end_member(reader);

    json_reader_read_member(reader, "thumburl");
    img->thumburl = g_strdup(json_reader_get_string_value(reader) ?: "");
    json_reader_end_member(reader);

    json_reader_read_member(reader, "copyright");
    img->copyright = g_strdup(json_reader_get_string_value(reader) ?: "");
    json_reader_end_member(reader);

    json_reader_read_member(reader, "reldate");
    img->reldate = g_strdup(json_reader_get_string_value(reader) ?: "");
    json_reader_end_member(reader);

    json_reader_read_member(reader, "story");
    img->story = g_strdup(json_reader_get_string_value(reader) ?: "");
    json_reader_end_member(reader);

    json_reader_read_member(reader, "width");
    img->width = (int)json_reader_get_int_value(reader);
    json_reader_end_member(reader);

    json_reader_read_member(reader, "height");
    img->height = (int)json_reader_get_int_value(reader);
    json_reader_end_member(reader);

    g_debug("解析 data 成功: title=\"%s\", imgurl=\"%s\"",
            img->title, img->imgurl);
    return img;
}

/* ---- 核心：请求 API 并解析 ---- */
static ImageData *api_request(const char *path) {
    g_debug("API 请求 path=%s", path);

    char *url = g_strdup_printf("%s%s", BASE_URL, path);
    char *body = http_get(url);
    g_free(url);

    if (!body) {
        g_debug("API 响应为空 -> 返回 NULL");
        return NULL;
    }

    JsonParser *parser = json_parser_new();
    GError *error = NULL;

    if (!json_parser_load_from_data(parser, body, -1, &error)) {
        g_warning("JSON 解析失败: %s", error->message);
        g_clear_error(&error);
        g_free(body);
        g_object_unref(parser);
        return NULL;
    }
    g_free(body);

    JsonReader *reader = json_reader_new(json_parser_get_root(parser));
    ImageData *img = NULL;

    /* 进入根节点并检查 status */
    if (json_reader_read_member(reader, "status")) {
        gint64 status = json_reader_get_int_value(reader);

        if (status == 1) {
            g_debug("API status=1, 进入 data 节点");
            json_reader_end_member(reader);

            if (json_reader_read_member(reader, "data")) {
                img = parse_image_data(reader);
                json_reader_end_member(reader);
            } else {
                g_warning("API 响应缺少 data 字段");
            }
        } else {
            g_warning("API status=%ld (非 1)", (long)status);
        }
    } else {
        g_warning("API 响应缺少 status 字段");
    }

    g_object_unref(reader);
    g_object_unref(parser);
    return img;
}

/* ---- 公开接口 ---- */

ImageData *api_today(const char *provider) {
    g_debug("api_today(provider=%s)", provider);
    char *path = g_strdup_printf("/%s/today?json=1", provider);
    ImageData *img = api_request(path);
    g_free(path);
    if (img)
        g_debug("api_today 成功: %s", img->title);
    return img;
}

ImageData *api_random(const char *provider) {
    g_debug("api_random(provider=%s)", provider);
    char *path = g_strdup_printf("/%s/random?json=1", provider);
    ImageData *img = api_request(path);
    g_free(path);
    if (img)
        g_debug("api_random 成功: %s", img->title);
    return img;
}

/* ---- 二进制下载回调 ---- */
static size_t curl_binary_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
    GByteArray *arr = userdata;
    g_byte_array_append(arr, (guint8 *)ptr, size * nmemb);
    return size * nmemb;
}

/* ---- 下载图片到 GBytes（二进制安全）---- */
GBytes *api_download_image(const char *url) {
    if (!url || !*url) {
        g_debug("api_download_image: URL 为空，跳过");
        return NULL;
    }

    g_debug("下载图片: %s", url);

    CURL *curl = curl_easy_init();
    if (!curl) {
        g_warning("curl_easy_init() 失败（图片下载）");
        return NULL;
    }

    GByteArray *buf = g_byte_array_new();

    /* 伪装浏览器 User-Agent，防止 CDN 返回 403
     * 注意：不要加 Referer，部分 CDN（dtstatic/duitang）会因跨域拒绝 */
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers,
        "User-Agent: Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36");

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_binary_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, buf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);

    CURLcode res = curl_easy_perform(curl);

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    double total_time = 0;
    curl_easy_getinfo(curl, CURLINFO_TOTAL_TIME, &total_time);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        g_warning("图片下载失败: %s", curl_easy_strerror(res));
        g_byte_array_unref(buf);
        return NULL;
    }

    if (http_code != 200) {
        g_warning("图片下载 HTTP %ld", http_code);
        g_byte_array_unref(buf);
        return NULL;
    }

    g_debug("图片下载完成: %u 字节, 耗时 %.2fs", buf->len, total_time);
    return g_byte_array_free_to_bytes(buf);
}

/* ---- 释放 ---- */
void api_image_free(ImageData *img) {
    if (!img) return;
    g_debug("释放 ImageData: title=\"%s\"", img->title);
    g_free(img->title);
    g_free(img->imgurl);
    g_free(img->thumburl);
    g_free(img->copyright);
    g_free(img->reldate);
    g_free(img->story);
    g_free(img);
}
