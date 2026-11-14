#include "xiaozhi_http.h"

#include "xiaozhi_common.h"

#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"

#include "xiaozhi_appmode.h"
#include "xiaozhi_camera.h"
#include "xiaozhi_camview.h"
#include "xiaozhi_ota.h"
#include "xiaozhi_recorder.h"
#include "xiaozhi_sdcard.h"

#define TAG "http"

#if XIAOZHI_HTTP_ENABLE

/* ---------------- 静态资源 ---------------- */

static httpd_handle_t s_server = NULL;
static char s_url[40] = "";

/* MJPEG 帧：tap 写 s_frame_buf，发送任务拷到 s_frame_tx 再往外发 */
static SemaphoreHandle_t s_frame_lock = NULL;
static SemaphoreHandle_t s_frame_sem = NULL;
static uint8_t *s_frame_buf = NULL;
static uint8_t *s_frame_tx = NULL;
static size_t s_frame_len = 0;
static uint32_t s_frame_seq = 0;
static volatile bool s_stream_active = false;

static QueueHandle_t s_stream_q = NULL;
static TaskHandle_t s_stream_task = NULL;

/* ---------------- HTML ---------------- */

static const char *HTML_HEAD =
    "<!DOCTYPE html><html lang=\"zh-CN\"><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>AI小智 · 局域网</title><style>"
    "body{margin:0;padding:12px;background:#16181c;color:#e6e6e6;"
    "font:14px/1.6 -apple-system,'PingFang SC','Microsoft YaHei',sans-serif}"
    "a{color:#5aa9ff;text-decoration:none}a:active{opacity:.6}"
    "h3{margin:6px 0 10px;font-size:17px}"
    "h4{margin:16px 0 6px;font-size:13px;color:#9aa4b2;font-weight:600}"
    ".card{background:#1e2126;border:1px solid #2b3038;border-radius:10px;padding:10px;margin-bottom:10px}"
    ".row{display:flex;gap:8px;align-items:center;justify-content:space-between;"
    "padding:7px 0;border-bottom:1px solid #262b33}"
    ".row:last-child{border-bottom:0}"
    ".nm{overflow:hidden;text-overflow:ellipsis;white-space:nowrap}"
    ".sz{color:#8b93a1;font-size:12px;flex:none;margin-left:8px}"
    ".hint{color:#9aa4b2;font-size:12px}"
    ".live{width:100%;border-radius:8px;display:block;background:#000}"
    "</style></head><body>";

static const char *HTML_TAIL = "</body></html>";

/* ---------------- 小工具 ---------------- */

static esp_err_t http_chunk_printf(httpd_req_t *req, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n <= 0) {
        return ESP_OK;
    }
    if (n >= (int)sizeof(buf)) {
        n = (int)sizeof(buf) - 1;
    }
    return httpd_resp_send_chunk(req, buf, n);
}

/* 文件名里可能有 < & " 等字符：网页里统一转义，避免 XSS/排版错乱 */
static void http_html_escape(const char *src, char *dst, size_t cap)
{
    size_t o = 0;
    for (const char *p = src; p != NULL && *p != '\0' && o + 8 < cap; p++) {
        switch (*p) {
            case '<':
                memcpy(dst + o, "&lt;", 4);
                o += 4;
                break;
            case '>':
                memcpy(dst + o, "&gt;", 4);
                o += 4;
                break;
            case '&':
                memcpy(dst + o, "&amp;", 5);
                o += 5;
                break;
            case '"':
                memcpy(dst + o, "&quot;", 6);
                o += 6;
                break;
            case '\'':
                memcpy(dst + o, "&#39;", 5);
                o += 5;
                break;
            default:
                dst[o++] = *p;
                break;
        }
    }
    dst[o] = '\0';
}

/* URL 编码：放进 href/query 的值（保留 / - . _ ~，其余 %XX） */
static void http_url_encode(const char *src, char *dst, size_t cap)
{
    static const char *hex = "0123456789ABCDEF";
    size_t o = 0;
    for (const char *p = src; p != NULL && *p != '\0' && o + 4 < cap; p++) {
        unsigned char c = (unsigned char)*p;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '/' || c == '-' ||
            c == '.' || c == '_' || c == '~') {
            dst[o++] = (char)c;
        } else {
            dst[o++] = '%';
            dst[o++] = hex[(c >> 4) & 0x0F];
            dst[o++] = hex[c & 0x0F];
        }
    }
    dst[o] = '\0';
}

static int http_hex_val(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/* URL 解码（%XX 与 +）；返回 false 表示非法编码或超长 */
static bool http_url_decode(const char *src, char *dst, size_t cap)
{
    size_t o = 0;
    for (const char *p = src; p != NULL && *p != '\0'; p++) {
        if (o + 1 >= cap) {
            return false;
        }
        if (*p == '%') {
            int hi = http_hex_val(p[1]);
            int lo = (hi < 0) ? -1 : http_hex_val(p[2]);
            if (hi < 0 || lo < 0) {
                return false;
            }
            dst[o++] = (char)((hi << 4) | lo);
            p += 2;
        } else if (*p == '+') {
            dst[o++] = ' ';
        } else {
            dst[o++] = *p;
        }
    }
    dst[o] = '\0';
    return true;
}

/* 只接受 /sdcard 下的相对路径：拒绝空、绝对路径与 ".."（防目录穿越） */
static bool http_path_ok(const char *rel)
{
    if (rel == NULL || rel[0] == '\0' || rel[0] == '/') {
        return false;
    }
    if (strstr(rel, "..") != NULL || strchr(rel, ':') != NULL) {
        return false;
    }
    return true;
}

/* 拼出 "/sdcard/<rel>"，rel 为空时得到 "/sdcard/" */
static void http_full_path(char *dst, size_t cap, const char *rel)
{
    if (rel == NULL || rel[0] == '\0') {
        snprintf(dst, cap, "%s", XIAOZHI_SD_MOUNT_POINT);
    } else {
        snprintf(dst, cap, "%s/%s", XIAOZHI_SD_MOUNT_POINT, rel);
    }
}

static const char *http_mime_of(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (dot == NULL) {
        return "application/octet-stream";
    }
    if (strcasecmp(dot, ".avi") == 0) {
        return "video/x-msvideo";
    }
    if (strcasecmp(dot, ".jpg") == 0 || strcasecmp(dot, ".jpeg") == 0) {
        return "image/jpeg";
    }
    if (strcasecmp(dot, ".csv") == 0 || strcasecmp(dot, ".txt") == 0 || strcasecmp(dot, ".log") == 0) {
        return "text/plain; charset=utf-8";
    }
    if (strcasecmp(dot, ".json") == 0) {
        return "application/json";
    }
    return "application/octet-stream";
}

static void http_size_str(uint64_t bytes, char *dst, size_t cap)
{
    if (bytes >= 1024ULL * 1024) {
        snprintf(dst, cap, "%.1f MB", (double)bytes / (1024.0 * 1024.0));
    } else if (bytes >= 1024ULL) {
        snprintf(dst, cap, "%.1f KB", (double)bytes / 1024.0);
    } else {
        snprintf(dst, cap, "%u B", (unsigned)bytes);
    }
}

/* ---------------- 目录列表 ---------------- */

/* 渲染一行为"文件/目录"，name 已转义、href 已编码 */
static esp_err_t http_row_file(httpd_req_t *req, const char *name, const char *rel, uint64_t size)
{
    char esc[256];
    char enc[640];
    char sz[24];
    http_html_escape(name, esc, sizeof(esc));
    http_url_encode(rel, enc, sizeof(enc));
    http_size_str(size, sz, sizeof(sz));
    return http_chunk_printf(req,
                             "<div class=\"row\"><a class=\"nm\" href=\"/download?path=%s\">%s</a>"
                             "<span class=\"sz\">%s</span></div>",
                             enc, esc, sz);
}

static esp_err_t http_row_dir(httpd_req_t *req, const char *name, const char *rel)
{
    char esc[256];
    char enc[640];
    http_html_escape(name, esc, sizeof(esc));
    http_url_encode(rel, enc, sizeof(enc));
    return http_chunk_printf(req, "<div class=\"row\"><a class=\"nm\" href=\"/list?dir=%s\">%s/</a></div>", enc, esc);
}

/* 列出 rel 目录（相对 /sdcard；"" 表示卡根目录）：先目录后文件，各一次遍历 */
static esp_err_t http_send_dir_list(httpd_req_t *req, const char *rel)
{
    char full[220];
    http_full_path(full, sizeof(full), rel);

    DIR *d = opendir(full);
    if (d == NULL) {
        return http_chunk_printf(req, "<div class=\"row\"><span class=\"hint\">打不开目录：%s</span></div>", full);
    }

    esp_err_t err = ESP_OK;
    for (int pass = 0; pass < 2 && err == ESP_OK; pass++) {
        rewinddir(d);
        struct dirent *ent = NULL;
        while ((ent = readdir(d)) != NULL) {
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
                continue;
            }

            char child_rel[512];
            if (rel == NULL || rel[0] == '\0') {
                snprintf(child_rel, sizeof(child_rel), "%s", ent->d_name);
            } else {
                snprintf(child_rel, sizeof(child_rel), "%s/%s", rel, ent->d_name);
            }

            char child_full[576];
            http_full_path(child_full, sizeof(child_full), child_rel);

            struct stat st;
            if (stat(child_full, &st) != 0) {
                continue;
            }

            bool is_dir = S_ISDIR(st.st_mode);
            if (pass == 0) {
                if (!is_dir) {
                    continue;
                }
                err = http_row_dir(req, ent->d_name, child_rel);
            } else {
                if (is_dir) {
                    continue;
                }
                err = http_row_file(req, ent->d_name, child_rel, (uint64_t)st.st_size);
            }
            if (err != ESP_OK) {
                break;
            }
        }
    }

    closedir(d);
    return err;
}

/* ---------------- 首页 ---------------- */

static esp_err_t index_handler(httpd_req_t *req)
{
    xiaozhi_rec_status_t st;
    xiaozhi_recorder_get_status(&st);

    httpd_resp_set_type(req, "text/html; charset=utf-8");

    esp_err_t err = httpd_resp_send_chunk(req, HTML_HEAD, strlen(HTML_HEAD));
    if (err != ESP_OK) {
        return err;
    }

    err = http_chunk_printf(req, "<h3>AI 小智 · 局域网</h3>"
                                  "<div class=\"card\">模式 <b>%s</b> · 录像 <b>%s</b> · 文件 <b>%u</b>"
                                  " · 帧率 <b>%.1f</b> · SD 剩余 <b>%llu MB</b></div>",
                             xiaozhi_appmode_name(xiaozhi_appmode_get()), xiaozhi_recorder_state_name(st.state),
                             (unsigned)st.files, st.fps,
                             (unsigned long long)(xiaozhi_sdcard_free_bytes_cached() / (1024ULL * 1024ULL)));
    if (err != ESP_OK) {
        return err;
    }
    if (st.file[0] != '\0') {
        err = http_chunk_printf(req, "<div class=\"card hint\">正在写：%s</div>", st.file);
        if (err != ESP_OK) {
            return err;
        }
    }

    /* 模式切换（远程）：点了之后 302 回首页，JS 轮询发现 mode 变化自动刷新 */
    const char *cur = xiaozhi_appmode_name(xiaozhi_appmode_get());
    err = http_chunk_printf(req,
                            "<h4>模式切换</h4><div class=\"card\"><b>%s</b>（当前）&nbsp;|&nbsp;"
                            "<a href=\"/api/mode?set=voice\">语音</a>&nbsp;"
                            "<a href=\"/api/mode?set=doorbell\">门铃</a>&nbsp;"
                            "<a href=\"/api/mode?set=monitor\">监控</a>&nbsp;"
                            "<a href=\"/api/mode?set=next\">下一个</a></div>"
                            "<script>var cur=\"%s\";setInterval(function(){"
                            "fetch('/api/status').then(function(r){return r.json()})"
                            ".then(function(j){if(j.mode!==cur){location.reload()}})"
                            ".catch(function(e){})},2000);</script>",
                            cur, cur);
    if (err != ESP_OK) {
        return err;
    }

    /* 实时预览：只有监控模式（摄像头已启动）才有源 */
    if (xiaozhi_camview_is_open()) {
        err = http_chunk_printf(req, "<img class=\"live\" src=\"/stream\" alt=\"实时预览\">"
                                     "<div class=\"hint\">MJPEG 实时预览（同一帧同步给本机屏幕）</div>");
    } else {
        err = http_chunk_printf(req, "<div class=\"card hint\">实时预览只在<b>监控模式</b>可用，"
                                     "请按 SW2 或对语音说“切换到监控模式”。</div>");
    }
    if (err != ESP_OK) {
        return err;
    }

    if (!xiaozhi_sdcard_cache_ready()) {
        err = http_chunk_printf(req, "<h4>SD 卡</h4><div class=\"card hint\">SD 未挂载或正忙（无卡/重挂载/读写异常），"
                                     "文件列表暂不可用。</div>");
    } else {
        err = httpd_resp_send_chunk(req, "<h4>SD 卡目录</h4><div class=\"card\">", strlen("<h4>SD 卡目录</h4><div class=\"card\">"));
        if (err == ESP_OK) {
            err = http_send_dir_list(req, "");
        }
        if (err == ESP_OK) {
            err = httpd_resp_send_chunk(req, "</div>", 6);
        }
    }
    if (err != ESP_OK) {
        return err;
    }

    err = http_chunk_printf(req, "<div class=\"hint\">状态接口：<a href=\"/api/status\">/api/status</a></div>");
    if (err == ESP_OK) {
        err = httpd_resp_send_chunk(req, HTML_TAIL, strlen(HTML_TAIL));
    }
    if (err == ESP_OK) {
        err = httpd_resp_send_chunk(req, NULL, 0);
    }
    return err;
}

/* ---------------- 目录浏览页 ---------------- */

static esp_err_t list_handler(httpd_req_t *req)
{
    char query[256];
    char raw[192];
    char rel[192] = "";

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "dir", raw, sizeof(raw)) == ESP_OK) {
        if (!http_url_decode(raw, rel, sizeof(rel)) || (rel[0] != '\0' && !http_path_ok(rel))) {
            httpd_resp_set_status(req, "400 Bad Request");
            httpd_resp_set_type(req, "text/plain; charset=utf-8");
            return httpd_resp_sendstr(req, "非法路径\n");
        }
    }

    if (!xiaozhi_sdcard_cache_ready()) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "text/html; charset=utf-8");
        return httpd_resp_sendstr(req, "<h3>SD 卡正忙或未就绪</h3><p>请稍后重试或检查卡。</p>");
    }

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    esp_err_t err = httpd_resp_send_chunk(req, HTML_HEAD, strlen(HTML_HEAD));
    if (err != ESP_OK) {
        return err;
    }

    char esc[192];
    http_html_escape(rel[0] ? rel : "/", esc, sizeof(esc));
    err = http_chunk_printf(req, "<h3>%s</h3><div class=\"card\">", esc);
    if (err != ESP_OK) {
        return err;
    }

    if (rel[0] != '\0') {
        /* 上一级 */
        char parent[192];
        snprintf(parent, sizeof(parent), "%s", rel);
        char *slash = strrchr(parent, '/');
        if (slash != NULL) {
            *slash = '\0';
        } else {
            parent[0] = '\0';
        }
        err = http_row_dir(req, "..", parent);
    }
    if (err == ESP_OK) {
        err = http_send_dir_list(req, rel);
    }
    if (err == ESP_OK) {
        static const char *tail = "</div><div class=\"hint\"><a href=\"/\">返回首页</a></div>";
        err = httpd_resp_send_chunk(req, tail, strlen(tail));
    }
    if (err == ESP_OK) {
        err = httpd_resp_send_chunk(req, HTML_TAIL, strlen(HTML_TAIL));
    }
    if (err == ESP_OK) {
        err = httpd_resp_send_chunk(req, NULL, 0);
    }
    return err;
}

/* ---------------- 目录 JSON（给安卓 App 用） ---------------- */

static void http_json_escape(const char *src, char *dst, size_t cap)
{
    size_t o = 0;
    if (cap == 0) {
        return;
    }
    for (const char *p = src; *p != '\0' && o + 8 < cap; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '"' || c == '\\') {
            dst[o++] = '\\';
            dst[o++] = (char)c;
        } else if (c == '\n') {
            dst[o++] = '\\';
            dst[o++] = 'n';
        } else if (c == '\r') {
            dst[o++] = '\\';
            dst[o++] = 'r';
        } else if (c == '\t') {
            dst[o++] = '\\';
            dst[o++] = 't';
        } else if (c < 0x20) {
            o += (size_t)snprintf(dst + o, cap - o, "\\u%04x", (unsigned)c);
        } else {
            dst[o++] = (char)c;
        }
    }
    dst[o] = '\0';
}

static esp_err_t http_json_entry(httpd_req_t *req, const char *name, const char *rel,
                                 bool is_dir, uint64_t size, bool *first)
{
    char esc_name[256];
    char esc_rel[640];
    http_json_escape(name, esc_name, sizeof(esc_name));
    http_json_escape(rel, esc_rel, sizeof(esc_rel));
    esp_err_t err = http_chunk_printf(req,
                                      "%s{\"name\":\"%s\",\"path\":\"%s\",\"dir\":%s,\"size\":%llu}",
                                      *first ? "" : ",", esc_name, esc_rel,
                                      is_dir ? "true" : "false", (unsigned long long)size);
    *first = false;
    return err;
}

/* GET /api/list?dir=<相对路径> -> [{name,path,dir,size}, ...]（先目录后文件）
 * 安卓 App 的文件浏览页用它；网页仍用 /list 的 HTML 页面。 */
static esp_err_t api_list_handler(httpd_req_t *req)
{
    char query[256];
    char raw[192];
    char rel[192] = "";

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "dir", raw, sizeof(raw)) == ESP_OK) {
        if (!http_url_decode(raw, rel, sizeof(rel)) || (rel[0] != '\0' && !http_path_ok(rel))) {
            httpd_resp_set_status(req, "400 Bad Request");
            httpd_resp_set_type(req, "application/json");
            return httpd_resp_sendstr(req, "{\"error\":\"bad path\"}");
        }
    }

    /* 卡正忙/异常时直接返回，避免在 httpd 任务里阻塞 FATFS 拖死整个网页服务 */
    if (!xiaozhi_sdcard_cache_ready()) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"error\":\"sd busy\"}");
    }

    char full[220];
    http_full_path(full, sizeof(full), rel);
    DIR *d = opendir(full);
    if (d == NULL) {
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        return httpd_resp_sendstr(req, "[]");
    }

    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    esp_err_t err = httpd_resp_send_chunk(req, "[", 1);
    bool first = true;
    for (int pass = 0; pass < 2 && err == ESP_OK; pass++) {
        rewinddir(d);
        struct dirent *ent = NULL;
        while ((ent = readdir(d)) != NULL) {
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
                continue;
            }

            char child_rel[512];
            if (rel[0] == '\0') {
                snprintf(child_rel, sizeof(child_rel), "%s", ent->d_name);
            } else {
                snprintf(child_rel, sizeof(child_rel), "%s/%s", rel, ent->d_name);
            }

            char child_full[576];
            http_full_path(child_full, sizeof(child_full), child_rel);

            struct stat st;
            if (stat(child_full, &st) != 0) {
                continue;
            }

            bool is_dir = S_ISDIR(st.st_mode);
            if ((pass == 0) != is_dir) {
                continue;
            }
            err = http_json_entry(req, ent->d_name, child_rel, is_dir, (uint64_t)st.st_size, &first);
            if (err != ESP_OK) {
                break;
            }
        }
    }

    closedir(d);
    if (err == ESP_OK) {
        err = httpd_resp_send_chunk(req, "]", 1);
    }
    if (err == ESP_OK) {
        err = httpd_resp_send_chunk(req, NULL, 0);
    }
    return err;
}

/* ---------------- 文件下载 ---------------- */

static esp_err_t download_handler(httpd_req_t *req)
{
    char query[256];
    char raw[192];
    char rel[192];

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "path", raw, sizeof(raw)) != ESP_OK || !http_url_decode(raw, rel, sizeof(rel)) ||
        !http_path_ok(rel)) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "text/plain; charset=utf-8");
        return httpd_resp_sendstr(req, "缺少或非法的 path 参数\n");
    }

    char full[260];
    http_full_path(full, sizeof(full), rel);

    struct stat st;
    if (stat(full, &st) != 0 || !S_ISREG(st.st_mode)) {
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_set_type(req, "text/plain; charset=utf-8");
        return httpd_resp_sendstr(req, "文件不存在\n");
    }

    FILE *fp = fopen(full, "rb");
    if (fp == NULL) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "text/plain; charset=utf-8");
        return httpd_resp_sendstr(req, "打开文件失败\n");
    }

    /* 下载分块缓冲放 PSRAM：内部 RAM 要留给 DMA，这里每次下载都会申请一块 */
    uint8_t *buf = (uint8_t *)heap_caps_malloc(XIAOZHI_HTTP_TX_CHUNK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        buf = (uint8_t *)malloc(XIAOZHI_HTTP_TX_CHUNK);
    }
    if (buf == NULL) {
        fclose(fp);
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "内存不足\n");
    }

    const char *base = strrchr(full, '/');
    base = (base != NULL) ? base + 1 : full;

    httpd_resp_set_type(req, http_mime_of(base));
    char disp[320];
    snprintf(disp, sizeof(disp), "attachment; filename=\"%s\"", base);
    httpd_resp_set_hdr(req, "Content-Disposition", disp);

    esp_err_t err = ESP_OK;
    size_t total = 0;
    while (total < (size_t)st.st_size) {
        size_t n = fread(buf, 1, XIAOZHI_HTTP_TX_CHUNK, fp);
        if (n == 0) {
            break; /* 卡被拔走/读失败 */
        }
        err = httpd_resp_send_chunk(req, (const char *)buf, n);
        if (err != ESP_OK) {
            break; /* 客户端断开 */
        }
        total += n;
    }

    free(buf);
    fclose(fp);

    if (err == ESP_OK) {
        err = httpd_resp_send_chunk(req, NULL, 0);
    }
    ESP_LOGI(TAG, "download %s: %u/%u bytes (%s)", rel, (unsigned)total, (unsigned)st.st_size,
             (err == ESP_OK) ? "ok" : "aborted");
    return err;
}

/* ---------------- 状态 JSON ---------------- */

static esp_err_t status_handler(httpd_req_t *req)
{
    xiaozhi_rec_status_t st;
    xiaozhi_recorder_get_status(&st);

    /* ver/slot：固件版本与当前启动槽（App 首页与 OTA 页都会读） */
    char buf[640];
    snprintf(buf, sizeof(buf),
             "{\"mode\":\"%s\",\"rec\":\"%s\",\"file\":\"%s\",\"frames\":%u,\"fps\":%.1f,\"dropped\":%u,"
             "\"files\":%u,\"reason\":\"%s\",\"sd_ready\":%s,\"sd_free_mb\":%llu,\"stream\":%s,"
             "\"ver\":\"%s\",\"slot\":\"%s\",\"ota\":\"%s\"}",
             xiaozhi_appmode_name(xiaozhi_appmode_get()), xiaozhi_recorder_state_name(st.state), st.file,
             (unsigned)st.frames, st.fps, (unsigned)st.dropped, (unsigned)st.files, st.reason,
             xiaozhi_sdcard_ready() ? "true" : "false",
             (unsigned long long)(xiaozhi_sdcard_free_bytes_cached() / (1024ULL * 1024ULL)),
             s_stream_active ? "true" : "false", xiaozhi_ota_version(), xiaozhi_ota_slot(),
             xiaozhi_ota_busy() ? "busy" : "idle");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, buf);
}

/* ---------------- 模式切换 API（网页远程切模式，M6 验收用） ---------------- */

static int http_mode_from_name(const char *name)
{
    if (strcmp(name, "voice") == 0) {
        return (int)XIAOZHI_MODE_VOICE;
    }
    if (strcmp(name, "doorbell") == 0) {
        return (int)XIAOZHI_MODE_DOORBELL;
    }
    if (strcmp(name, "monitor") == 0) {
        return (int)XIAOZHI_MODE_MONITOR;
    }
    return -1;
}

/* GET /api/mode?set=voice|doorbell|monitor|next
 * 走 xiaozhi_appmode 的 REMOTE 来源，与按键/语音同一仲裁队列，互不影响。
 * 切换是异步的：请求入队即返回 302 回首页，页面由 JS 轮询 /api/status
 * 发现 mode 变化后自动刷新，监控模式下自动挂出 <img src="/stream">。 */
static esp_err_t mode_handler(httpd_req_t *req)
{
    char query[64];
    char raw[32];
    char set[32] = "";

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "set", raw, sizeof(raw)) == ESP_OK) {
        if (!http_url_decode(raw, set, sizeof(set)) || set[0] == '\0') {
            httpd_resp_set_status(req, "400 Bad Request");
            httpd_resp_set_type(req, "text/plain; charset=utf-8");
            return httpd_resp_sendstr(req, "非法参数\n");
        }
    }

    esp_err_t err;
    if (strcmp(set, "next") == 0) {
        err = xiaozhi_appmode_request_next(XIAOZHI_MODE_SRC_REMOTE);
    } else {
        int mode = http_mode_from_name(set);
        if (mode < 0) {
            httpd_resp_set_status(req, "400 Bad Request");
            httpd_resp_set_type(req, "text/plain; charset=utf-8");
            return httpd_resp_sendstr(req, "set 仅支持 voice/doorbell/monitor/next\n");
        }
        err = xiaozhi_appmode_request((xiaozhi_mode_t)mode, XIAOZHI_MODE_SRC_REMOTE);
    }

    if (err != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "text/plain; charset=utf-8");
        return httpd_resp_sendstr(req, "模式切换请求入队失败\n");
    }

    ESP_LOGI(TAG, "mode switch requested via web: %s", set);
    /* 302 回首页：浏览器点完按钮直接回到带预览的页面 */
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, "redirect\n");
}

/* ==========================================================================
 * OTA 固件升级接口（实现在 main/ota/xiaozhi_ota.c）
 *   GET  /api/ota/list               列 /sdcard/OTA 下的 .bin（含镜像里的版本号）
 *   POST /api/ota/apply?path=OTA/x.bin  用 SD 上的包安装（阻塞到写完并重启）
 *   POST /api/ota/upload?name=x.bin&save=1  手机推包：边收边写 flash，同时落卡
 *   GET  /api/ota/status             进度/状态
 *   POST /api/ota/reboot             手动重启
 * 注意：apply / upload 都在 httpd 任务内同步执行，所以进入前先停掉监控预览，
 *       避免 SD + flash + 摄像头同时抢占资源。
 * ========================================================================== */

static esp_err_t http_ota_json(httpd_req_t *req, const char *status, const char *json)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, json);
}

/* 把 src 追加到 dst[*off]；返回 false 表示放不下。
 * 不用 snprintf("%s")：src 长度编译期未知时会触发 -Wformat-truncation，本工程按错误处理。 */
static bool http_ota_append(char *dst, size_t cap, size_t *off, const char *src)
{
    if (src == NULL) {
        return true;
    }
    size_t n = strlen(src);
    if (*off + n >= cap) {
        return false;
    }
    memcpy(dst + *off, src, n);
    *off += n;
    dst[*off] = '\0';
    return true;
}



/* 文件名清洗：只允许 [0-9A-Za-z._-]，其余替换成 '_'，并强制 .bin 后缀。
 * 目的是防止路径穿越、FATFS 长文件名/中文名在 URL 里被编码出问题。 */
static void http_ota_sanitize_name(const char *in, char *out, size_t cap)
{
    size_t o = 0;
    const char *base = in;
    if (base != NULL) {
        const char *slash = strrchr(base, '/');
        const char *bslash = strrchr(base, '\\');
        if (slash != NULL) {
            base = slash + 1;
        }
        if (bslash != NULL && bslash + 1 > base) {
            base = bslash + 1;
        }
    }
    if (base == NULL || base[0] == '\0') {
        snprintf(out, cap, "upload.bin");
        return;
    }
    for (const char *p = base; *p != '\0' && o + 1 < cap; p++) {
        char c = *p;
        bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '.' || c == '_' ||
                  c == '-';
        out[o++] = ok ? c : '_';
    }
    out[o] = '\0';
    if (o < 5 || strcasecmp(out + o - 4, ".bin") != 0) {
        if (o + 5 <= cap - 1) {
            snprintf(out + o, cap - o, ".bin");
        }
    }
}

/* GET /api/ota/list */
static esp_err_t ota_list_handler(httpd_req_t *req)
{
    /* static：httpd 任务栈只有 4~8 KB，8 条包信息（约 100 B/条）放栈上太冒险 */
    static xiaozhi_ota_pkg_t pkgs[XIAOZHI_OTA_MAX_PKGS];
    int n = xiaozhi_ota_list(pkgs, XIAOZHI_OTA_MAX_PKGS);

    /* static：httpd 任务栈很小（4~8 KB），1 KB 级 JSON 缓冲不能放栈上 */
    static char buf[1400];
    size_t off = 0;
    buf[0] = '\0';
    bool ok = http_ota_append(buf, sizeof(buf), &off, "{\"ver\":\"") &&
              http_ota_append(buf, sizeof(buf), &off, xiaozhi_ota_version()) &&
              http_ota_append(buf, sizeof(buf), &off, "\",\"slot\":\"") &&
              http_ota_append(buf, sizeof(buf), &off, xiaozhi_ota_slot()) &&
              http_ota_append(buf, sizeof(buf), &off, "\",\"pkgs\":[");
    if (!ok) {
        return http_ota_json(req, "500 Internal Server Error", "{\"ok\":false,\"err\":\"json overflow\"}");
    }
    for (int i = 0; i < n; i++) {
        /* 单条：全部是定长数组/字面量，snprintf 不会触发截断告警 */
        char item[256];
        int w = snprintf(item, sizeof(item),
                         "%s{\"name\":\"%s\",\"path\":\"%s/%s\",\"size\":%u,\"ver\":\"%s\",\"valid\":%s}",
                         (i == 0) ? "" : ",", pkgs[i].name, XIAOZHI_OTA_DIR_REL, pkgs[i].name,
                         (unsigned)pkgs[i].size, pkgs[i].version, pkgs[i].valid ? "true" : "false");
        if (w < 0 || (size_t)w >= sizeof(item)) {
            continue;
        }
        if (!http_ota_append(buf, sizeof(buf), &off, item)) {
            break; /* 理论上到不了：最多 8 条 × ~150 B < 1.4 KB */
        }
    }
    (void)http_ota_append(buf, sizeof(buf), &off, "]}");
    return http_ota_json(req, "200 OK", buf);
}

/* GET /api/ota/status */
static esp_err_t ota_status_handler(httpd_req_t *req)
{
    xiaozhi_ota_status_t st;
    xiaozhi_ota_get_status(&st);

    /* msg(96) + src(64) + 其它字段 → 384 够；全部走 http_ota_append 避免 %s 截断告警 */
    char buf[384];
    char num[24];
    size_t off = 0;
    buf[0] = '\0';
    bool ok = http_ota_append(buf, sizeof(buf), &off, "{\"state\":\"") &&
              http_ota_append(buf, sizeof(buf), &off, xiaozhi_ota_state_name(st.state)) &&
              http_ota_append(buf, sizeof(buf), &off, "\",\"pct\":");
    if (ok) {
        snprintf(num, sizeof(num), "%d", st.pct);
        ok = http_ota_append(buf, sizeof(buf), &off, num);
    }
    if (ok) {
        snprintf(num, sizeof(num), ",\"done\":%u,\"total\":%u", (unsigned)st.bytes_done, (unsigned)st.bytes_total);
        ok = http_ota_append(buf, sizeof(buf), &off, num);
    }
    if (ok) {
        ok = http_ota_append(buf, sizeof(buf), &off, ",\"msg\":\"") &&
             http_ota_append(buf, sizeof(buf), &off, st.msg) &&
             http_ota_append(buf, sizeof(buf), &off, "\",\"src\":\"") &&
             http_ota_append(buf, sizeof(buf), &off, st.src) &&
             http_ota_append(buf, sizeof(buf), &off, "\",\"ver\":\"") &&
             http_ota_append(buf, sizeof(buf), &off, xiaozhi_ota_version()) &&
             http_ota_append(buf, sizeof(buf), &off, "\",\"slot\":\"") &&
             http_ota_append(buf, sizeof(buf), &off, xiaozhi_ota_slot()) &&
             http_ota_append(buf, sizeof(buf), &off, "\"}");
    }
    if (!ok) {
        return http_ota_json(req, "500 Internal Server Error", "{\"ok\":false,\"err\":\"json overflow\"}");
    }
    return http_ota_json(req, "200 OK", buf);
}

/* POST /api/ota/apply?path=OTA/xxx.bin（同步：写完 → 校验 → 切分区 → 1.5 s 后重启） */
static esp_err_t ota_apply_handler(httpd_req_t *req)
{
    if (xiaozhi_ota_busy()) {
        return http_ota_json(req, "409 Conflict", "{\"ok\":false,\"err\":\"正在升级中，请稍候\"}");
    }

    char query[256];
    char raw[160];
    char path[160] = "";
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "path", raw, sizeof(raw)) != ESP_OK ||
        !http_url_decode(raw, path, sizeof(path)) || path[0] == '\0') {
        return http_ota_json(req, "400 Bad Request", "{\"ok\":false,\"err\":\"缺少 path 参数\"}");
    }
    if (strstr(path, "..") != NULL) {
        return http_ota_json(req, "400 Bad Request", "{\"ok\":false,\"err\":\"非法路径\"}");
    }

    ESP_LOGW(TAG, "ota apply requested: %s", path);
    esp_err_t err = xiaozhi_ota_apply_path(path);
    if (err != ESP_OK) {
        xiaozhi_ota_status_t st;
        xiaozhi_ota_get_status(&st);
        char errbuf[64];
        esp_err_to_name_r(err, errbuf, sizeof(errbuf));
        char buf[192];
        size_t eoff = 0;
        buf[0] = '\0';
        (void)http_ota_append(buf, sizeof(buf), &eoff, "{\"ok\":false,\"err\":\"");
        (void)http_ota_append(buf, sizeof(buf), &eoff, (st.msg[0] != '\0') ? st.msg : errbuf);
        (void)http_ota_append(buf, sizeof(buf), &eoff, "\"}");
        return http_ota_json(req, "400 Bad Request", buf);
    }

    char buf[192];
    char num[24];
    size_t off = 0;
    buf[0] = '\0';
    bool ok = http_ota_append(buf, sizeof(buf), &off, "{\"ok\":true,\"ver\":\"") &&
              http_ota_append(buf, sizeof(buf), &off, xiaozhi_ota_version()) &&
              http_ota_append(buf, sizeof(buf), &off, "\",\"slot\":\"") &&
              http_ota_append(buf, sizeof(buf), &off, xiaozhi_ota_slot()) &&
              http_ota_append(buf, sizeof(buf), &off, "\",\"reboot_ms\":");
    if (ok) {
        snprintf(num, sizeof(num), "%d", XIAOZHI_OTA_REBOOT_DELAY_MS);
        (void)http_ota_append(buf, sizeof(buf), &off, num);
    }
    (void)http_ota_append(buf, sizeof(buf), &off, "}");
    return http_ota_json(req, "200 OK", buf);
}

/* POST /api/ota/upload?name=xx.bin&save=1   请求体 = 应用分区镜像原始字节 */
static esp_err_t ota_upload_handler(httpd_req_t *req)
{
    if (xiaozhi_ota_busy()) {
        return http_ota_json(req, "409 Conflict", "{\"ok\":false,\"err\":\"正在升级中，请稍候\"}");
    }

    char query[256];
    char raw[128];
    char raw_name[96] = "";
    char name[64];
    bool save = true;

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        if (httpd_query_key_value(query, "name", raw, sizeof(raw)) == ESP_OK) {
            (void)http_url_decode(raw, raw_name, sizeof(raw_name));
        }
        if (httpd_query_key_value(query, "save", raw, sizeof(raw)) == ESP_OK) {
            save = (raw[0] != '0');
        }
    }
    http_ota_sanitize_name(raw_name, name, sizeof(name));

    size_t total = req->content_len;
    if (total == 0) {
        return http_ota_json(req, "400 Bad Request", "{\"ok\":false,\"err\":\"请求体为空\"}");
    }

    esp_err_t err = xiaozhi_ota_upload_begin(name, (uint32_t)total, save);
    if (err != ESP_OK) {
        xiaozhi_ota_status_t st;
        xiaozhi_ota_get_status(&st);
        char errbuf[64];
        esp_err_to_name_r(err, errbuf, sizeof(errbuf));
        char buf[224];
        size_t eoff = 0;
        buf[0] = '\0';
        (void)http_ota_append(buf, sizeof(buf), &eoff, "{\"ok\":false,\"err\":\"");
        (void)http_ota_append(buf, sizeof(buf), &eoff, (st.msg[0] != '\0') ? st.msg : errbuf);
        (void)http_ota_append(buf, sizeof(buf), &eoff, "\"}");
        return http_ota_json(req,
                             (err == ESP_ERR_INVALID_STATE) ? "409 Conflict"
                                                            : ((err == ESP_ERR_INVALID_SIZE) ? "400 Bad Request"
                                                                                             : "500 Internal Server Error"),
                             buf);
    }

    uint8_t *buf = xiaozhi_ota_chunk_buf();
    size_t chunk = xiaozhi_ota_chunk_size();
    if (buf == NULL || chunk == 0) {
        xiaozhi_ota_upload_abort();
        return http_ota_json(req, "500 Internal Server Error", "{\"ok\":false,\"err\":\"缓冲不可用\"}");
    }

    /* 手机端可能中途卡顿：单次 recv 超时（config.recv_wait_timeout=10s）后重试，
     * 连续 5 次都收不到才放弃（总容忍约 50 s 停顿） */
    size_t remain = total;
    int timeouts = 0;
    while (remain > 0) {
        size_t want = (remain > chunk) ? chunk : remain;
        int n = httpd_req_recv(req, (char *)buf, want);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            if (++timeouts > 5) {
                ESP_LOGE(TAG, "ota upload: recv timeout too many times, abort");
                xiaozhi_ota_upload_abort();
                return http_ota_json(req, "408 Request Timeout", "{\"ok\":false,\"err\":\"接收超时\"}");
            }
            continue;
        }
        if (n <= 0) {
            ESP_LOGE(TAG, "ota upload: recv failed (%d), abort", n);
            xiaozhi_ota_upload_abort();
            return http_ota_json(req, "400 Bad Request", "{\"ok\":false,\"err\":\"连接中断\"}");
        }
        timeouts = 0;
        err = xiaozhi_ota_upload_write(buf, (size_t)n);
        if (err != ESP_OK) {
            xiaozhi_ota_upload_abort();
            return http_ota_json(req, "500 Internal Server Error", "{\"ok\":false,\"err\":\"写入 flash 失败\"}");
        }
        remain -= (size_t)n;
    }

    err = xiaozhi_ota_upload_end();
    if (err != ESP_OK) {
        xiaozhi_ota_status_t st;
        xiaozhi_ota_get_status(&st);
        char errbuf[64];
        esp_err_to_name_r(err, errbuf, sizeof(errbuf));
        char json[224];
        size_t eoff = 0;
        json[0] = '\0';
        (void)http_ota_append(json, sizeof(json), &eoff, "{\"ok\":false,\"err\":\"");
        (void)http_ota_append(json, sizeof(json), &eoff, (st.msg[0] != '\0') ? st.msg : errbuf);
        (void)http_ota_append(json, sizeof(json), &eoff, "\"}");
        return http_ota_json(req, "400 Bad Request", json);
    }

    char json[224];
    char num[24];
    size_t off = 0;
    json[0] = '\0';
    bool ok = http_ota_append(json, sizeof(json), &off, "{\"ok\":true,\"ver\":\"") &&
              http_ota_append(json, sizeof(json), &off, xiaozhi_ota_version()) &&
              http_ota_append(json, sizeof(json), &off, "\",\"reboot_ms\":");
    if (ok) {
        snprintf(num, sizeof(num), "%d", XIAOZHI_OTA_REBOOT_DELAY_MS);
        (void)http_ota_append(json, sizeof(json), &off, num);
    }
    if (ok) {
        (void)http_ota_append(json, sizeof(json), &off, save ? ",\"saved\":true}" : ",\"saved\":false}");
    }
    return http_ota_json(req, "200 OK", json);
}

/* POST /api/ota/reboot：新手固件已切好启动分区，手动重启立即生效 */
static esp_err_t ota_reboot_handler(httpd_req_t *req)
{
    xiaozhi_ota_schedule_reboot(300);
    return http_ota_json(req, "200 OK", "{\"ok\":true}");
}

/* ---------------- MJPEG 实时预览 ---------------- */

/* 预览任务每帧回调（同步执行）：只做一次 memcpy，绝不能阻塞 */
static void http_stream_on_frame(const uint8_t *jpeg, size_t len, int w, int h, void *ctx)
{
    (void)w;
    (void)h;
    (void)ctx;
    if (!s_stream_active || len == 0 || len > XIAOZHI_HTTP_FRAME_MAX) {
        return;
    }
    if (xSemaphoreTake(s_frame_lock, 0) != pdTRUE) {
        return; /* 上一帧还没被发送任务取走，直接丢这一帧 */
    }
    memcpy(s_frame_buf, jpeg, len);
    s_frame_len = len;
    s_frame_seq++;
    xSemaphoreGive(s_frame_lock);
    xSemaphoreGive(s_frame_sem);
}

static esp_err_t http_stream_reject(httpd_req_t *req, const char *msg)
{
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_sendstr(req, msg);
}

/* 真正干活：在一个独立任务里跑，避免长连接堵住 httpd 任务 */
static void http_serve_stream(httpd_req_t *req)
{
    /* 升级期间必须让出带宽与 flash/SD，直接拒绝预览 */
    if (xiaozhi_ota_busy()) {
        (void)http_stream_reject(req, "设备正在升级固件，请稍后再试\n");
        return;
    }
    if (!xiaozhi_camera_ready() || !xiaozhi_camview_is_open()) {
        (void)http_stream_reject(req, "摄像头未启动：请先切到监控模式\n");
        return;
    }
    if (s_frame_buf == NULL || s_frame_tx == NULL || s_frame_lock == NULL || s_frame_sem == NULL) {
        (void)http_stream_reject(req, "服务器内存不足\n");
        return;
    }

    /* 清掉可能残留的信号量，再挂上订阅 */
    while (xSemaphoreTake(s_frame_sem, 0) == pdTRUE) {
    }
    if (xSemaphoreTake(s_frame_lock, pdMS_TO_TICKS(200)) == pdTRUE) {
        s_frame_len = 0;
        s_frame_seq = 0;
        xSemaphoreGive(s_frame_lock);
    }

    if (xiaozhi_camview_set_frame_tap(XIAOZHI_CAMVIEW_TAP_STREAM, http_stream_on_frame, NULL) != ESP_OK) {
        (void)http_stream_reject(req, "挂接预览失败\n");
        return;
    }
    s_stream_active = true;

    httpd_resp_set_type(req, "multipart/x-mixed-replace; boundary=frame");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    ESP_LOGI(TAG, "stream start");
    int64_t t0 = esp_timer_get_time();
    uint32_t sent = 0;
    int idle = 0;

    for (;;) {
        if (xSemaphoreTake(s_frame_sem, pdMS_TO_TICKS(1000)) != pdTRUE) {
            /* 2s 内没有新帧：模式切走 / 摄像头停了都会走到这里 */
            if (!xiaozhi_camview_is_open()) {
                ESP_LOGW(TAG, "stream: preview closed, stop");
                break;
            }
            if (++idle >= 5) {
                ESP_LOGW(TAG, "stream: no frame for 5s, stop");
                break;
            }
            continue;
        }
        idle = 0;

        uint32_t seq = 0;
        size_t len = 0;
        if (xSemaphoreTake(s_frame_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
            seq = s_frame_seq;
            len = s_frame_len;
            if (len > 0 && len <= XIAOZHI_HTTP_FRAME_MAX) {
                /* 前 128 字节留给 multipart 头，避免合并时再拷一次 JPEG */
                memcpy(s_frame_tx + 128, s_frame_buf, len);
            } else {
                len = 0;
            }
            xSemaphoreGive(s_frame_lock);
        }
        if (len == 0) {
            continue;
        }

        char hdr[128];
        int n = snprintf(hdr, sizeof(hdr),
                         "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\nX-Seq: %u\r\n\r\n",
                         (unsigned)len, (unsigned)seq);
        /* snprintf 返回“想要写入的长度”，n >= sizeof(hdr) 表示已被截断，头部不可信。
         * 同时确认 n + len + 2 不超过 s_frame_tx 的分配量（FRAME_MAX + 512）。 */
        if (n < 0 || n >= (int)sizeof(hdr) ||
            ((size_t)n + len + 2) > (XIAOZHI_HTTP_FRAME_MAX + 512)) {
            ESP_LOGW(TAG, "stream: header overflow (n=%d len=%u)", n, (unsigned)len);
            break;
        }
        /* 头 + JPEG + CRLF 合并成一次 send_chunk。原来分 3 次发（头部几十字节、JPEG、
         * 2 字节 CRLF）在部分客户端会因延迟 ACK/Nagle 被拖到 ~1 帧/秒。 */
        memmove(s_frame_tx + n, s_frame_tx + 128, len);
        memcpy(s_frame_tx, hdr, (size_t)n);
        s_frame_tx[n + len] = '\r';
        s_frame_tx[n + len + 1] = '\n';
        if (httpd_resp_send_chunk(req, (const char *)s_frame_tx, (size_t)n + len + 2) != ESP_OK) {
            ESP_LOGI(TAG, "stream: client gone");
            break;
        }
        sent++;
    }

    (void)xiaozhi_camview_set_frame_tap(XIAOZHI_CAMVIEW_TAP_STREAM, NULL, NULL);
    s_stream_active = false;
    (void)httpd_resp_send_chunk(req, NULL, 0);

    int64_t dur_us = esp_timer_get_time() - t0;
    ESP_LOGI(TAG, "stream stop: %u frames in %lld ms (%.1f fps)", (unsigned)sent, (long long)(dur_us / 1000),
             (dur_us > 0) ? ((float)sent * 1000000.0f / (float)dur_us) : 0.0f);
}

static void http_stream_task(void *arg)
{
    (void)arg;
    httpd_req_t *req = NULL;
    for (;;) {
        if (xQueueReceive(s_stream_q, &req, portMAX_DELAY) != pdTRUE || req == NULL) {
            continue;
        }
        http_serve_stream(req);
        (void)httpd_req_async_handler_complete(req);
        req = NULL;
    }
}

/* /stream 入口：立刻转交独立任务，返回后 httpd 继续服务其它请求 */
static esp_err_t stream_handler(httpd_req_t *req)
{
    if (!xiaozhi_camera_ready() || !xiaozhi_camview_is_open()) {
        return http_stream_reject(req, "摄像头未启动：请先切到监控模式\n");
    }
    /* 显式单客户端判定：不要只看队列深度。上一路客户端异常断开时，处理任务最多要等
     * 一次 send 超时或 5 s 空闲才会清 s_stream_active，期间直接拒绝并明确告知。 */
    if (s_stream_active) {
        return http_stream_reject(req, "已有预览客户端在连接\n");
    }

    httpd_req_t *arq = NULL;
    esp_err_t err = httpd_req_async_handler_begin(req, &arq);
    if (err != ESP_OK || arq == NULL) {
        ESP_LOGE(TAG, "async handler begin failed: %s", esp_err_to_name(err));
        return ESP_FAIL;
    }

    if (s_stream_q == NULL || xQueueSend(s_stream_q, &arq, 0) != pdTRUE) {
        (void)http_stream_reject(arq, "已有预览客户端在连接\n");
        (void)httpd_req_async_handler_complete(arq);
        return ESP_OK;
    }
    return ESP_OK;
}

/* ---------------- 初始化 ---------------- */

/* 内存余量诊断：内部 RAM 在 SR/音频初始化后很紧，httpd 栈分配失败时靠这行定位 */
static void http_log_heap(const char *when)
{
    ESP_LOGI(TAG, "heap@%s: internal free=%u KB (largest=%u KB), psram free=%u KB", when,
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024),
             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
}

/* 重新读取 netif 的实际 IP 填进 s_url。
 * refresh_only=true 时不打日志（给 xiaozhi_http_url() 每次调用用，避免刷屏） */
static void http_update_url(bool log_result)
{
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip = {0};
    if (netif != NULL && esp_netif_get_ip_info(netif, &ip) == ESP_OK && ip.ip.addr != 0) {
        char cur[40];
        snprintf(cur, sizeof(cur), "http://" IPSTR "/", IP2STR(&ip.ip));
        if (strcmp(cur, s_url) != 0) {
            snprintf(s_url, sizeof(s_url), "%s", cur);
            if (log_result) {
                ESP_LOGI(TAG, "web ready: %s （手机浏览器直接访问）", s_url);
            }
        }
    } else {
        if (s_url[0] != '\0' || log_result) {
            s_url[0] = '\0';
            if (log_result) {
                ESP_LOGW(TAG, "no wifi ip yet, web server still listening on port %d", XIAOZHI_HTTP_PORT);
            }
        }
    }
}

esp_err_t xiaozhi_http_init(void)
{
    if (s_server != NULL) {
        return ESP_OK;
    }

    s_frame_buf = (uint8_t *)heap_caps_malloc(XIAOZHI_HTTP_FRAME_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    /* 发送缓冲多留 512 字节：要把 multipart 头 + JPEG + CRLF 合并成一次 send */
    s_frame_tx = (uint8_t *)heap_caps_malloc(XIAOZHI_HTTP_FRAME_MAX + 512, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_frame_lock = xSemaphoreCreateMutex();
    s_frame_sem = xSemaphoreCreateBinary();
    s_stream_q = xQueueCreate(1, sizeof(httpd_req_t *));
    if (s_frame_buf == NULL || s_frame_tx == NULL || s_frame_lock == NULL || s_frame_sem == NULL ||
        s_stream_q == NULL) {
        ESP_LOGE(TAG, "buffer/queue alloc failed, web disabled");
        return ESP_ERR_NO_MEM;
    }

    /* 栈放 PSRAM（xiaozhi_task_create 统一处理，失败自动回落内部 RAM），
     * 把内部 RAM 让给 httpd 任务与 mbedtls AES-DMA。 */
    if (xiaozhi_task_create(http_stream_task, "http_stream", XIAOZHI_STACK_5K, NULL, XIAOZHI_PRIO_NORMAL,
                            &s_stream_task, XIAOZHI_HTTP_STREAM_CORE) != pdPASS) {
        ESP_LOGE(TAG, "create stream task failed, web disabled");
        return ESP_ERR_NO_MEM;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = XIAOZHI_HTTP_PORT;
    /* 原 10 个，新增 5 个 OTA 路由（list/apply/upload/status/reboot）后调到 16 */
    config.max_uri_handlers = 16;
    config.max_open_sockets = XIAOZHI_HTTP_MAX_SOCKETS;
    config.lru_purge_enable = true;
    /* 对端异常断开时尽快让 send 失败，避免推流任务卡在发不出去的 socket 上占住名额 */
    config.send_wait_timeout = 3;
    /* recv 超时只在 OTA 上传（POST 大包）时用得上，放宽到 10 s 以适应手机端抖动 */
    config.recv_wait_timeout = 10;

    /* httpd 任务栈必须留在内部 RAM。实测（2026-11-06）此刻内部最大连续块仅 ~7KB，
     * 直接要 8192 会 ESP_ERR_HTTPD_TASK；且 httpd_start 失败会残留监听 socket，
     * 同进程再重试会 EADDRINUSE(112)。因此只做单次创建：按“当前最大连续块-1KB”取栈，
     * 下限 4096（IDF 默认值，足够 chunked HTML + FATFS 4KB 分块读取）。 */
    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t stack = XIAOZHI_HTTP_STACK;
    if (largest < stack) {
        stack = (largest > XIAOZHI_HTTP_STACK_MIN + 1024) ? (largest - 1024) : XIAOZHI_HTTP_STACK_MIN;
    }
    config.stack_size = stack;
    http_log_heap("httpd_start");
    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed (stack=%u): %s, web disabled", (unsigned)stack, esp_err_to_name(err));
        s_server = NULL;
        return err;
    }
    ESP_LOGI(TAG, "httpd started with stack=%u B", (unsigned)stack);

    static const httpd_uri_t uri_index = {.uri = "/", .method = HTTP_GET, .handler = index_handler, .user_ctx = NULL};
    static const httpd_uri_t uri_index2 = {.uri = "/index.html", .method = HTTP_GET, .handler = index_handler, .user_ctx = NULL};
    static const httpd_uri_t uri_list = {.uri = "/list", .method = HTTP_GET, .handler = list_handler, .user_ctx = NULL};
    static const httpd_uri_t uri_dl = {.uri = "/download", .method = HTTP_GET, .handler = download_handler, .user_ctx = NULL};
    static const httpd_uri_t uri_status = {.uri = "/api/status", .method = HTTP_GET, .handler = status_handler, .user_ctx = NULL};
    static const httpd_uri_t uri_api_list = {.uri = "/api/list", .method = HTTP_GET, .handler = api_list_handler, .user_ctx = NULL};
    static const httpd_uri_t uri_mode = {.uri = "/api/mode", .method = HTTP_GET, .handler = mode_handler, .user_ctx = NULL};
    static const httpd_uri_t uri_stream = {.uri = "/stream", .method = HTTP_GET, .handler = stream_handler, .user_ctx = NULL};
    /* OTA（M7）：手机推送 + SD 卡包安装 */
    static const httpd_uri_t uri_ota_list = {.uri = "/api/ota/list", .method = HTTP_GET, .handler = ota_list_handler, .user_ctx = NULL};
    static const httpd_uri_t uri_ota_status = {.uri = "/api/ota/status", .method = HTTP_GET, .handler = ota_status_handler, .user_ctx = NULL};
    static const httpd_uri_t uri_ota_apply = {.uri = "/api/ota/apply", .method = HTTP_POST, .handler = ota_apply_handler, .user_ctx = NULL};
    static const httpd_uri_t uri_ota_upload = {.uri = "/api/ota/upload", .method = HTTP_POST, .handler = ota_upload_handler, .user_ctx = NULL};
    static const httpd_uri_t uri_ota_reboot = {.uri = "/api/ota/reboot", .method = HTTP_POST, .handler = ota_reboot_handler, .user_ctx = NULL};

    (void)httpd_register_uri_handler(s_server, &uri_index);
    (void)httpd_register_uri_handler(s_server, &uri_index2);
    (void)httpd_register_uri_handler(s_server, &uri_list);
    (void)httpd_register_uri_handler(s_server, &uri_dl);
    (void)httpd_register_uri_handler(s_server, &uri_status);
    (void)httpd_register_uri_handler(s_server, &uri_api_list);
    (void)httpd_register_uri_handler(s_server, &uri_mode);
    (void)httpd_register_uri_handler(s_server, &uri_stream);
    (void)httpd_register_uri_handler(s_server, &uri_ota_list);
    (void)httpd_register_uri_handler(s_server, &uri_ota_status);
    (void)httpd_register_uri_handler(s_server, &uri_ota_apply);
    (void)httpd_register_uri_handler(s_server, &uri_ota_upload);
    (void)httpd_register_uri_handler(s_server, &uri_ota_reboot);

    http_update_url(true);
    ESP_LOGI(TAG, "init ok: port=%d, frame=%d KB, stream by xiaozhi_camview tap", XIAOZHI_HTTP_PORT,
             XIAOZHI_HTTP_FRAME_MAX / 1024);
    return ESP_OK;
}

const char *xiaozhi_http_url(void)
{
    /* 每次调用都重读 netif：DHCP 续租/重连换 IP 后，屏幕上显示的地址也是实际地址
     * （用户要求"地址必须是实际地址，不能写死"） */
    http_update_url(false);
    return s_url;
}

bool xiaozhi_http_stream_active(void)
{
    return s_stream_active;
}

#else /* XIAOZHI_HTTP_ENABLE == 0：空实现 */

esp_err_t xiaozhi_http_init(void)
{
    return ESP_OK;
}
const char *xiaozhi_http_url(void)
{
    return "";
}
bool xiaozhi_http_stream_active(void)
{
    return false;
}

#endif /* XIAOZHI_HTTP_ENABLE */
