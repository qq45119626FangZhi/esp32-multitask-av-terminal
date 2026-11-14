#include "xiaozhi_ota.h"

#if XIAOZHI_OTA_ENABLE

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "xiaozhi_appmode.h"
#include "xiaozhi_lvgl.h"
#include "xiaozhi_sdcard.h"

#define TAG "ota"

/* 每个分块 4 KB：与 flash 扇区对齐，缓冲区必须放**内部 RAM**
 * （esp_ota_write 内部会短暂关 cache，PSRAM 缓冲/PSRAM 栈都不可用） */
#define OTA_CHUNK 4096

/* app 镜像里 esp_app_desc_t 的偏移：image header(24) + segment header(8) */
#define OTA_APP_DESC_OFFSET 0x20

static SemaphoreHandle_t s_lock = NULL;
static xiaozhi_ota_status_t s_st;

static const esp_partition_t *s_part = NULL; /* 本次写入的目标 OTA 分区 */
static esp_ota_handle_t s_handle;            /* esp_ota 句柄 */
static bool s_handle_open = false;

static FILE *s_sd = NULL;   /* 手机推包时同时落卡的副本 */
static char s_sd_path[96];
static int s_sd_err = 0;    /* 落卡失败后不再重试，但升级继续 */

static int s_last_pct = -1; /* 进度条去重：只在百分比变化时刷屏 */

static char s_version[40] = "0.0.0";
static char s_version_short[12] = "V0.0";
static char s_slot[16] = "--";

/* 分块缓冲：静态 .bss（内部 RAM），供 httpd handler 收包与 SD 读包共用。
 * 两条路径由 xiaozhi_ota_busy() 互斥，不会同时使用。 */
static uint8_t s_chunk[OTA_CHUNK];

uint8_t *xiaozhi_ota_chunk_buf(void)
{
    return s_chunk;
}

size_t xiaozhi_ota_chunk_size(void)
{
    return sizeof(s_chunk);
}

/* ---------------- 小工具 ---------------- */

static void ota_lock(void)
{
    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
}

static void ota_unlock(void)
{
    if (s_lock != NULL) {
        xSemaphoreGive(s_lock);
    }
}

/* 安全拷贝/追加：用 memcpy 显式截断。
 * 不用 snprintf(dst, n, "%s", src) —— 当 src 长度编译期未知时 GCC 会报
 * -Wformat-truncation，而本工程把它当错误（-Werror）。 */
static void ota_str_copy(char *dst, size_t cap, const char *src)
{
    if (dst == NULL || cap == 0) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    size_t n = strlen(src);
    if (n > cap - 1) {
        n = cap - 1;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static void ota_str_cat(char *dst, size_t cap, const char *src)
{
    if (dst == NULL || cap == 0 || src == NULL) {
        return;
    }
    size_t used = strnlen(dst, cap);
    if (used >= cap - 1) {
        return;
    }
    size_t n = strlen(src);
    if (n > cap - 1 - used) {
        n = cap - 1 - used;
    }
    memcpy(dst + used, src, n);
    dst[used + n] = '\0';
}

const char *xiaozhi_ota_state_name(xiaozhi_ota_state_t state)
{
    switch (state) {
        case XIAOZHI_OTA_STATE_RECEIVING:
            return "receiving";
        case XIAOZHI_OTA_STATE_WRITING:
            return "writing";
        case XIAOZHI_OTA_STATE_VERIFYING:
            return "verifying";
        case XIAOZHI_OTA_STATE_DONE:
            return "done";
        case XIAOZHI_OTA_STATE_ERROR:
            return "error";
        case XIAOZHI_OTA_STATE_IDLE:
        default:
            return "idle";
    }
}

/* 更新状态 + 刷新屏幕进度条（不持锁调用 LVGL，避免嵌套锁） */
static void ota_set_state(xiaozhi_ota_state_t state, int pct, const char *msg)
{
    ota_lock();
    s_st.state = state;
    if (pct >= 0) {
        s_st.pct = (pct > 100) ? 100 : pct;
    }
    if (msg != NULL) {
        ota_str_copy(s_st.msg, sizeof(s_st.msg), msg);
    }
    const char *show_msg = s_st.msg;
    int show_pct = s_st.pct;
    ota_unlock();

    /* 屏幕进度条：idle/error 由调用方决定是否收起 */
    if (state == XIAOZHI_OTA_STATE_RECEIVING || state == XIAOZHI_OTA_STATE_WRITING ||
        state == XIAOZHI_OTA_STATE_VERIFYING || state == XIAOZHI_OTA_STATE_DONE) {
        if (show_pct != s_last_pct || state != XIAOZHI_OTA_STATE_WRITING) {
            s_last_pct = show_pct;
            xiaozhi_lvgl_ota_progress_show(show_pct, show_msg);
        }
    }
}

/* 进度推进（每 4 KB 调一次；只在百分比变化时刷屏） */
static void ota_set_progress(uint32_t done, uint32_t total, bool show)
{
    int pct = (total > 0) ? (int)(((uint64_t)done * 100ULL) / (uint64_t)total) : 0;
    if (pct > 100) {
        pct = 100;
    }
    ota_lock();
    s_st.bytes_done = done;
    s_st.bytes_total = total;
    s_st.pct = pct;
    int changed = (pct != s_last_pct);
    xiaozhi_ota_state_t st = s_st.state;
    char msg[96];
    ota_str_copy(msg, sizeof(msg), (st == XIAOZHI_OTA_STATE_RECEIVING) ? "接收中" : s_st.msg);
    ota_unlock();

    if (show && changed) {
        s_last_pct = pct;
        xiaozhi_lvgl_ota_progress_show(pct, msg);
    }
}

/* 取 app 镜像头里的版本号（相对 /sdcard 的路径） */
static bool ota_read_image_version(const char *full_path, char *ver, size_t ver_len, char *name, size_t name_len)
{
    FILE *f = fopen(full_path, "rb");
    if (f == NULL) {
        return false;
    }
    bool ok = false;
    esp_app_desc_t desc;
    if (fseek(f, OTA_APP_DESC_OFFSET, SEEK_SET) == 0 &&
        fread(&desc, 1, sizeof(desc), f) == sizeof(desc) && desc.magic_word == ESP_APP_DESC_MAGIC_WORD) {
        if (ver != NULL && ver_len > 0) {
            ota_str_copy(ver, ver_len, desc.version);
        }
        if (name != NULL && name_len > 0) {
            ota_str_copy(name, name_len, desc.project_name);
        }
        ok = true;
    }
    fclose(f);
    return ok;
}

/* 把相对路径规范成 /sdcard/... 的绝对路径，拒绝 ".." 与越界 */
static bool ota_make_full_path(const char *rel, char *out, size_t out_len)
{
    if (rel == NULL || rel[0] == '\0') {
        return false;
    }
    if (strstr(rel, "..") != NULL) {
        return false;
    }
    if (strncmp(rel, "/sdcard/", 8) == 0) {
        if (strlen(rel) >= out_len) {
            return false;
        }
        ota_str_copy(out, out_len, rel);
        return true;
    }
    while (*rel == '/') {
        rel++;
    }
    if (strchr(rel, '/') == NULL) {
        /* 只给文件名：默认放在 OTA 目录 */
        ota_str_copy(out, out_len, XIAOZHI_OTA_DIR);
    } else {
        ota_str_copy(out, out_len, xiaozhi_sdcard_root());
    }
    ota_str_cat(out, out_len, "/");
    ota_str_cat(out, out_len, rel);
    return true;
}

/* ---------------- 重启 ---------------- */

static void ota_reboot_cb(void *arg)
{
    (void)arg;
    ESP_LOGW(TAG, "reboot now for new firmware");
    esp_restart();
}

void xiaozhi_ota_schedule_reboot(uint32_t delay_ms)
{
    static esp_timer_handle_t s_timer = NULL;
    if (s_timer == NULL) {
        const esp_timer_create_args_t args = {
            .callback = ota_reboot_cb,
            .arg = NULL,
            .name = "ota_reboot",
        };
        if (esp_timer_create(&args, &s_timer) != ESP_OK) {
            ESP_LOGE(TAG, "create reboot timer failed, reboot immediately");
            esp_restart();
            return;
        }
    }
    if (esp_timer_start_once(s_timer, (uint64_t)delay_ms * 1000ULL) != ESP_OK) {
        esp_restart();
    } else {
        ESP_LOGW(TAG, "reboot in %u ms", (unsigned)delay_ms);
    }
}

/* ---------------- 初始化 / 版本 ---------------- */

void xiaozhi_ota_init(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
    }
    memset(&s_st, 0, sizeof(s_st));
    s_st.state = XIAOZHI_OTA_STATE_IDLE;
    snprintf(s_st.msg, sizeof(s_st.msg), "空闲");

    const esp_app_desc_t *desc = esp_app_get_description();
    if (desc != NULL) {
        ota_str_copy(s_version, sizeof(s_version), desc->version);
        /* "1.0.0" -> "V1.0"，只取前两段，便于屏幕上显示 */
        int a = 0;
        int b = 0;
        char tmp[48];
        if (sscanf(desc->version, "%d.%d", &a, &b) == 2) {
            snprintf(tmp, sizeof(tmp), "V%d.%d", a, b);
        } else {
            ota_str_copy(tmp, sizeof(tmp), "V");
            ota_str_cat(tmp, sizeof(tmp), desc->version);
        }
        ota_str_copy(s_version_short, sizeof(s_version_short), tmp);
    }

    const esp_partition_t *run = esp_ota_get_running_partition();
    if (run != NULL) {
        ota_str_copy(s_slot, sizeof(s_slot), run->label);
    }
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    ESP_LOGI(TAG, "version=%s slot=%s next=%s(%u KB) sd=%s", s_version, s_slot,
             (next != NULL) ? next->label : "none", (next != NULL) ? (unsigned)(next->size / 1024) : 0U,
             xiaozhi_sdcard_ready() ? "ready" : "none");
    if (next == NULL) {
        ESP_LOGE(TAG, "no OTA slot available, please migrate partitions.csv (see main/doc/OTA.md 5.3)");
    }

    if (xiaozhi_sdcard_ready()) {
        (void)xiaozhi_sdcard_ensure_dir(XIAOZHI_OTA_DIR);
    }
}

void xiaozhi_ota_mark_valid(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
    bool pending = (run != NULL) && (esp_ota_get_state_partition(run, &st) == ESP_OK) &&
                   (st == ESP_OTA_IMG_PENDING_VERIFY);
    /* 未开启回滚保护时该调用是空操作；开启时它把 PENDING_VERIFY 变成 VALID，
     * 否则下次重启 bootloader 会回退到旧分区（表现为"升级后总是不生效"） */
    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    ESP_LOGI(TAG, "mark app valid (%s): %s", pending ? "pending verify" : "normal", esp_err_to_name(err));
}

const char *xiaozhi_ota_version(void)
{
    return s_version;
}

const char *xiaozhi_ota_version_short(void)
{
    return s_version_short;
}

const char *xiaozhi_ota_slot(void)
{
    return s_slot;
}

bool xiaozhi_ota_busy(void)
{
    bool busy;
    ota_lock();
    busy = (s_st.state == XIAOZHI_OTA_STATE_RECEIVING || s_st.state == XIAOZHI_OTA_STATE_WRITING ||
            s_st.state == XIAOZHI_OTA_STATE_VERIFYING);
    ota_unlock();
    return busy;
}

void xiaozhi_ota_get_status(xiaozhi_ota_status_t *out)
{
    if (out == NULL) {
        return;
    }
    ota_lock();
    *out = s_st;
    ota_unlock();
}

/* ---------------- 收尾：校验 + 切启动分区 ---------------- */

/* handle 已 end/abort；成功则切换启动分区并计划重启 */
static esp_err_t ota_finish_success(void)
{
    esp_err_t err = esp_ota_set_boot_partition(s_part);
    if (err != ESP_OK) {
        ota_set_state(XIAOZHI_OTA_STATE_ERROR, 0, "切换启动分区失败");
        ESP_LOGE(TAG, "set boot partition failed: %s", esp_err_to_name(err));
        xiaozhi_lvgl_ota_progress_hide();
        return err;
    }
    char msg[96];
    snprintf(msg, sizeof(msg), "升级完成，正在重启…");
    s_last_pct = -1;
    ota_set_state(XIAOZHI_OTA_STATE_DONE, 100, msg);
    xiaozhi_lvgl_ota_progress_show(100, msg);
    ESP_LOGW(TAG, "OTA success: next boot from %s (%s)", s_part->label, s_st.src);
    xiaozhi_ota_schedule_reboot(XIAOZHI_OTA_REBOOT_DELAY_MS);
    return ESP_OK;
}

static esp_err_t ota_finish_error(const char *msg)
{
    ota_set_state(XIAOZHI_OTA_STATE_ERROR, 0, msg);
    ESP_LOGE(TAG, "OTA failed: %s", msg);
    xiaozhi_lvgl_ota_progress_hide();
    return ESP_FAIL;
}

/* 取消落卡副本（失败/中断时删掉半包，避免列表里出现坏包） */
static void ota_close_sd(bool remove_file)
{
    if (s_sd != NULL) {
        fflush(s_sd);
        fclose(s_sd);
        s_sd = NULL;
    }
    if (remove_file && s_sd_path[0] != '\0') {
        (void)unlink(s_sd_path);
    }
    s_sd_path[0] = '\0';
}

/* 升级开始前：切回语音模式，让摄像头/录像先停下来（避免和 flash 写抢资源） */
static void ota_quiet_before_write(void)
{
    if (!xiaozhi_appmode_is(XIAOZHI_MODE_VOICE)) {
        ESP_LOGI(TAG, "switch to voice mode before OTA");
        (void)xiaozhi_appmode_request(XIAOZHI_MODE_VOICE, XIAOZHI_MODE_SRC_REMOTE);
        /* 异步仲裁，给退出监控/停录像留一点时间 */
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

/* ---------------- 入口 1：手机推送 ---------------- */

esp_err_t xiaozhi_ota_upload_begin(const char *name, uint32_t total, bool save_to_sd)
{
    if (xiaozhi_ota_busy()) {
        return ESP_ERR_INVALID_STATE;
    }

    s_part = esp_ota_get_next_update_partition(NULL);
    if (s_part == NULL) {
        ota_set_state(XIAOZHI_OTA_STATE_ERROR, 0, "没有可用的 OTA 分区");
        ESP_LOGE(TAG, "upload: no OTA partition (migrate partitions.csv first)");
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (total > 0 && total > s_part->size) {
        char msg[96];
        snprintf(msg, sizeof(msg), "包太大：%u KB > 分区 %u KB", (unsigned)(total / 1024),
                 (unsigned)(s_part->size / 1024));
        ota_set_state(XIAOZHI_OTA_STATE_ERROR, 0, msg);
        return ESP_ERR_INVALID_SIZE;
    }

    ota_quiet_before_write();

    /* 统一用顺序写模式（边收边擦）：避免 begin 阶段一次性擦 3.8 MB 让进度条长时间停在 0% */
    esp_err_t err = esp_ota_begin(s_part, OTA_WITH_SEQUENTIAL_WRITES, &s_handle);
    if (err != ESP_OK) {
        /* 开启回滚保护后，若当前固件还没 mark valid，OTA 会被拒绝 */
        ota_set_state(XIAOZHI_OTA_STATE_ERROR, 0,
                      (err == ESP_ERR_OTA_ROLLBACK_INVALID_STATE) ? "固件尚未确认有效，请先重启一次"
                                                                  : "启动 OTA 写失败");
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        return err;
    }
    s_handle_open = true;

    ota_lock();
    memset(&s_st, 0, sizeof(s_st));
    s_st.state = XIAOZHI_OTA_STATE_RECEIVING;
    s_st.bytes_total = total;
    ota_str_copy(s_st.src, sizeof(s_st.src), (name != NULL && name[0] != '\0') ? name : "upload.bin");
    snprintf(s_st.msg, sizeof(s_st.msg), "接收中");
    ota_unlock();
    s_last_pct = -1;

    /* 落卡副本：Q2 需求 —— 手机推的包同时存到 /sdcard/OTA/，之后可在列表里再次点击安装 */
    s_sd_err = 0;
    s_sd_path[0] = '\0';
    if (save_to_sd) {
        if (!xiaozhi_sdcard_ready()) {
            ESP_LOGW(TAG, "sd not ready, skip saving package");
        } else {
            (void)xiaozhi_sdcard_ensure_dir(XIAOZHI_OTA_DIR);
            ota_str_copy(s_sd_path, sizeof(s_sd_path), XIAOZHI_OTA_DIR);
            ota_str_cat(s_sd_path, sizeof(s_sd_path), "/");
            ota_str_cat(s_sd_path, sizeof(s_sd_path),
                        (name != NULL && name[0] != '\0') ? name : "upload.bin");
            s_sd = fopen(s_sd_path, "wb");
            if (s_sd == NULL) {
                ESP_LOGW(TAG, "open %s failed, skip saving package", s_sd_path);
                s_sd_path[0] = '\0';
            } else {
                ESP_LOGI(TAG, "saving package to %s", s_sd_path);
            }
        }
    }

    xiaozhi_lvgl_ota_progress_show(0, "接收中");
    ESP_LOGI(TAG, "upload begin: %s total=%u B -> %s", s_st.src, (unsigned)total, s_part->label);
    return ESP_OK;
}

esp_err_t xiaozhi_ota_upload_write(const void *data, size_t len)
{
    if (!s_handle_open || data == NULL || len == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = esp_ota_write(s_handle, data, len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
        return err;
    }
    /* 同时写卡：失败不影响升级，只记一次日志后放弃落卡 */
    if (s_sd != NULL && s_sd_err == 0) {
        if (fwrite(data, 1, len, s_sd) != len) {
            ESP_LOGW(TAG, "write sd failed, stop saving package");
            s_sd_err = 1;
            ota_close_sd(true);
        }
    }

    ota_lock();
    s_st.bytes_done += (uint32_t)len;
    uint32_t done = s_st.bytes_done;
    uint32_t total = s_st.bytes_total;
    ota_unlock();
    ota_set_progress(done, total, true);
    return ESP_OK;
}

esp_err_t xiaozhi_ota_upload_end(void)
{
    if (!s_handle_open) {
        return ESP_ERR_INVALID_STATE;
    }
    ota_set_state(XIAOZHI_OTA_STATE_VERIFYING, 100, "校验镜像…");

    if (s_sd != NULL) {
        fflush(s_sd);
        if (ferror(s_sd)) {
            ESP_LOGW(TAG, "sd copy has error, remove it");
            ota_close_sd(true);
        } else {
            ota_close_sd(false);
        }
    }

    /* esp_ota_end 内部会校验镜像头/chip id/SHA256，并释放句柄 */
    esp_err_t err = esp_ota_end(s_handle);
    s_handle_open = false;
    if (err != ESP_OK) {
        return ota_finish_error("升级包无效（校验失败）");
    }
    return ota_finish_success();
}

void xiaozhi_ota_upload_abort(void)
{
    if (s_handle_open) {
        esp_ota_abort(s_handle);
        s_handle_open = false;
    }
    ota_close_sd(true);
    ota_set_state(XIAOZHI_OTA_STATE_ERROR, 0, "已取消");
    xiaozhi_lvgl_ota_progress_hide();
    ESP_LOGW(TAG, "upload aborted");
}

/* ---------------- 入口 2：SD 卡安装（阻塞调用，放在 httpd 任务里） ---------------- */

esp_err_t xiaozhi_ota_apply_path(const char *rel_path)
{
    if (xiaozhi_ota_busy()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!xiaozhi_sdcard_ready()) {
        ota_set_state(XIAOZHI_OTA_STATE_ERROR, 0, "SD 卡未挂载");
        xiaozhi_lvgl_ota_progress_hide();
        return ESP_ERR_INVALID_STATE;
    }

    char full[128];
    if (!ota_make_full_path(rel_path, full, sizeof(full))) {
        ota_set_state(XIAOZHI_OTA_STATE_ERROR, 0, "非法路径");
        xiaozhi_lvgl_ota_progress_hide();
        return ESP_ERR_INVALID_ARG;
    }

    struct stat sb;
    if (stat(full, &sb) != 0 || !S_ISREG(sb.st_mode) || sb.st_size <= 0) {
        ota_set_state(XIAOZHI_OTA_STATE_ERROR, 0, "升级包不存在");
        xiaozhi_lvgl_ota_progress_hide();
        return ESP_ERR_NOT_FOUND;
    }

    s_part = esp_ota_get_next_update_partition(NULL);
    if (s_part == NULL) {
        ota_set_state(XIAOZHI_OTA_STATE_ERROR, 0, "没有可用的 OTA 分区");
        xiaozhi_lvgl_ota_progress_hide();
        return ESP_ERR_NOT_SUPPORTED;
    }
    if ((uint32_t)sb.st_size > s_part->size) {
        ota_set_state(XIAOZHI_OTA_STATE_ERROR, 0, "升级包大于分区");
        xiaozhi_lvgl_ota_progress_hide();
        return ESP_ERR_INVALID_SIZE;
    }

    FILE *f = fopen(full, "rb");
    if (f == NULL) {
        ota_set_state(XIAOZHI_OTA_STATE_ERROR, 0, "打开升级包失败");
        xiaozhi_lvgl_ota_progress_hide();
        return ESP_ERR_NOT_FOUND;
    }

    ota_quiet_before_write();

    /* 顺序写：边读边擦，进度条能持续走 */
    esp_err_t err = esp_ota_begin(s_part, OTA_WITH_SEQUENTIAL_WRITES, &s_handle);
    if (err != ESP_OK) {
        fclose(f);
        ota_set_state(XIAOZHI_OTA_STATE_ERROR, 0,
                      (err == ESP_ERR_OTA_ROLLBACK_INVALID_STATE) ? "固件尚未确认有效，请先重启一次"
                                                                  : "启动 OTA 写失败");
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        xiaozhi_lvgl_ota_progress_hide();
        return err;
    }
    s_handle_open = true;

    const char *base = strrchr(full, '/');
    base = (base != NULL) ? (base + 1) : full;

    ota_lock();
    memset(&s_st, 0, sizeof(s_st));
    s_st.state = XIAOZHI_OTA_STATE_WRITING;
    s_st.bytes_total = (uint32_t)sb.st_size;
    ota_str_copy(s_st.src, sizeof(s_st.src), base);
    snprintf(s_st.msg, sizeof(s_st.msg), "写入中");
    ota_unlock();
    s_last_pct = -1;
    xiaozhi_lvgl_ota_progress_show(0, "写入中");
    ESP_LOGW(TAG, "apply begin: %s (%u B) -> %s", full, (unsigned)sb.st_size, s_part->label);

    /* 4 KB 内部 RAM 缓冲：flash 写入期间 cache 关闭，不能用 PSRAM 缓冲 */
    uint8_t *buf = s_chunk;

    uint32_t done = 0;
    err = ESP_OK;
    while (done < (uint32_t)sb.st_size) {
        size_t want = (size_t)sb.st_size - done;
        if (want > OTA_CHUNK) {
            want = OTA_CHUNK;
        }
        size_t got = fread(buf, 1, want, f);
        if (got == 0) {
            err = ESP_FAIL;
            ESP_LOGE(TAG, "read sd failed at %u", (unsigned)done);
            break;
        }
        err = esp_ota_write(s_handle, buf, got);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
            break;
        }
        done += (uint32_t)got;
        ota_lock();
        s_st.bytes_done = done;
        ota_unlock();
        ota_set_progress(done, (uint32_t)sb.st_size, true);
    }
    fclose(f);

    if (err != ESP_OK) {
        if (s_handle_open) {
            esp_ota_abort(s_handle);
            s_handle_open = false;
        }
        return ota_finish_error("写入 flash 失败");
    }

    ota_set_state(XIAOZHI_OTA_STATE_VERIFYING, 100, "校验镜像…");
    err = esp_ota_end(s_handle);
    s_handle_open = false;
    if (err != ESP_OK) {
        return ota_finish_error("升级包无效（校验失败）");
    }
    return ota_finish_success();
}

esp_err_t xiaozhi_ota_apply_async(const char *rel_path)
{
    /* 本工程里 apply 走 httpd 任务同步执行（httpd 任务栈在内部 RAM，
     * 满足 esp_ota_write 期间关 cache 的要求）；保留该接口给未来异步化用。 */
    static char s_pending[128];
    if (rel_path == NULL || strlen(rel_path) >= sizeof(s_pending)) {
        return ESP_ERR_INVALID_ARG;
    }
    ota_str_copy(s_pending, sizeof(s_pending), rel_path);
    return xiaozhi_ota_apply_path(s_pending);
}

/* ---------------- 列表 ---------------- */

static int ota_name_cmp(const char *a, const char *b)
{
    return strcmp(a, b);
}

int xiaozhi_ota_list(xiaozhi_ota_pkg_t *out, int max)
{
    if (out == NULL || max <= 0) {
        return 0;
    }
    memset(out, 0, sizeof(xiaozhi_ota_pkg_t) * (size_t)max);
    if (!xiaozhi_sdcard_ready()) {
        return 0;
    }
    (void)xiaozhi_sdcard_ensure_dir(XIAOZHI_OTA_DIR);

    DIR *dir = opendir(XIAOZHI_OTA_DIR);
    if (dir == NULL) {
        return 0;
    }

    int n = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL && n < max) {
        size_t len = strlen(ent->d_name);
        if (len < 5 || strcasecmp(ent->d_name + len - 4, ".bin") != 0) {
            continue;
        }
        char full[192];
        ota_str_copy(full, sizeof(full), XIAOZHI_OTA_DIR);
        ota_str_cat(full, sizeof(full), "/");
        ota_str_cat(full, sizeof(full), ent->d_name);
        struct stat sb;
        if (stat(full, &sb) != 0 || !S_ISREG(sb.st_mode)) {
            continue;
        }
        ota_str_copy(out[n].name, sizeof(out[n].name), ent->d_name);
        out[n].size = (uint32_t)sb.st_size;
        if (!ota_read_image_version(full, out[n].version, sizeof(out[n].version), NULL, 0)) {
            snprintf(out[n].version, sizeof(out[n].version), "--");
            out[n].valid = false;
        } else {
            out[n].valid = true;
        }
        n++;
    }
    closedir(dir);

    /* 按文件名排序（时间戳式命名即时间序），插入排序足够（最多 8 条） */
    for (int i = 1; i < n; i++) {
        xiaozhi_ota_pkg_t tmp = out[i];
        int j = i - 1;
        while (j >= 0 && ota_name_cmp(out[j].name, tmp.name) > 0) {
            out[j + 1] = out[j];
            j--;
        }
        out[j + 1] = tmp;
    }
    return n;
}

#else /* XIAOZHI_OTA_ENABLE == 0：空实现，保证链接期符号齐全 */

#include <stdio.h>

void xiaozhi_ota_init(void)
{
}
void xiaozhi_ota_mark_valid(void)
{
}
const char *xiaozhi_ota_version(void)
{
    return "0.0.0";
}
const char *xiaozhi_ota_version_short(void)
{
    return "V0.0";
}
const char *xiaozhi_ota_slot(void)
{
    return "--";
}
bool xiaozhi_ota_busy(void)
{
    return false;
}
void xiaozhi_ota_get_status(xiaozhi_ota_status_t *out)
{
    if (out != NULL) {
        out->state = XIAOZHI_OTA_STATE_IDLE;
    }
}
const char *xiaozhi_ota_state_name(xiaozhi_ota_state_t state)
{
    (void)state;
    return "idle";
}
esp_err_t xiaozhi_ota_upload_begin(const char *name, uint32_t total, bool save_to_sd)
{
    (void)name;
    (void)total;
    (void)save_to_sd;
    return ESP_ERR_NOT_SUPPORTED;
}
uint8_t *xiaozhi_ota_chunk_buf(void)
{
    return NULL;
}
size_t xiaozhi_ota_chunk_size(void)
{
    return 0;
}
esp_err_t xiaozhi_ota_upload_write(const void *data, size_t len)
{
    (void)data;
    (void)len;
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t xiaozhi_ota_upload_end(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}
void xiaozhi_ota_upload_abort(void)
{
}
esp_err_t xiaozhi_ota_apply_path(const char *rel_path)
{
    (void)rel_path;
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t xiaozhi_ota_apply_async(const char *rel_path)
{
    (void)rel_path;
    return ESP_ERR_NOT_SUPPORTED;
}
int xiaozhi_ota_list(xiaozhi_ota_pkg_t *out, int max)
{
    (void)out;
    (void)max;
    return 0;
}
void xiaozhi_ota_schedule_reboot(uint32_t delay_ms)
{
    (void)delay_ms;
}

#endif /* XIAOZHI_OTA_ENABLE */
