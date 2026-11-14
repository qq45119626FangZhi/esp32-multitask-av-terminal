#include "xiaozhi_camera.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "xiaozhi_camera_pins.h"
#include "xiaozhi_sdcard.h"

#define TAG "camera"

#if XIAOZHI_CAM_ENABLE

static SemaphoreHandle_t s_lock = NULL;
static bool s_inited = false;
static xiaozhi_cam_format_t s_fmt = XIAOZHI_CAM_FMT_JPEG;
static uint32_t s_snapshot_seq = 0;

static pixformat_t xiaozhi_camera_pixformat(xiaozhi_cam_format_t fmt) { return (fmt == XIAOZHI_CAM_FMT_RGB565) ? PIXFORMAT_RGB565 : PIXFORMAT_JPEG; }

/* 打印内部 SRAM / PSRAM 水位：内部 RAM 决定相机能否初始化，排查内存问题时先看这一行 */
static void xiaozhi_camera_log_heap(const char *when)
{
    ESP_LOGI(TAG, "%s: internal free=%u KB, largest=%u B, min=%u KB | psram free=%u KB, largest=%u B", when, (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL), (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
}

/* 按指定模式初始化相机。
 * esp_camera_set_psram_mode() 在"相机尚未初始化"时只置位标志并返回 ESP_ERR_INVALID_STATE，
 * 这里正是初始化前的用法，返回值忽略（若相机已初始化，它会做一次完整 reconfigure）。 */
static esp_err_t xiaozhi_camera_init_once(const camera_config_t *cfg, bool psram_dma)
{
    (void)esp_camera_set_psram_mode(psram_dma);
    return esp_camera_init(cfg);
}

/* 真正的 DVP + 传感器初始化：SCCB 复用 xiaozhi_audio_init() 建立的 I2C 总线 */
static esp_err_t xiaozhi_camera_start(xiaozhi_cam_format_t fmt)
{
    camera_config_t cfg = {0};
    cfg.pin_pwdn = XIAOZHI_CAM_PIN_PWDN;
    cfg.pin_reset = XIAOZHI_CAM_PIN_RESET;
    cfg.pin_xclk = XIAOZHI_CAM_PIN_XCLK;
    /* -1 表示不新建 I2C 总线，改用 sccb_i2c_port 指定的已存在总线 */
    cfg.pin_sccb_sda = XIAOZHI_CAM_PIN_SIOD;
    cfg.pin_sccb_scl = XIAOZHI_CAM_PIN_SIOC;
    cfg.sccb_i2c_port = XIAOZHI_CAM_SCCB_I2C_PORT;

    cfg.pin_d0 = XIAOZHI_CAM_PIN_D0;
    cfg.pin_d1 = XIAOZHI_CAM_PIN_D1;
    cfg.pin_d2 = XIAOZHI_CAM_PIN_D2;
    cfg.pin_d3 = XIAOZHI_CAM_PIN_D3;
    cfg.pin_d4 = XIAOZHI_CAM_PIN_D4;
    cfg.pin_d5 = XIAOZHI_CAM_PIN_D5;
    cfg.pin_d6 = XIAOZHI_CAM_PIN_D6;
    cfg.pin_d7 = XIAOZHI_CAM_PIN_D7;
    cfg.pin_vsync = XIAOZHI_CAM_PIN_VSYNC;
    cfg.pin_href = XIAOZHI_CAM_PIN_HREF;
    cfg.pin_pclk = XIAOZHI_CAM_PIN_PCLK;

    cfg.xclk_freq_hz = XIAOZHI_CAM_XCLK_HZ;
    /* S3 的 XCLK 由 LCD_CAM 外设输出，这两个字段仅为结构体兼容 */
    cfg.ledc_timer = LEDC_TIMER_0;
    cfg.ledc_channel = LEDC_CHANNEL_0;

    cfg.pixel_format = xiaozhi_camera_pixformat(fmt);
    cfg.frame_size = FRAMESIZE_QVGA; /* 320x240 */
    cfg.jpeg_quality = XIAOZHI_CAM_JPEG_QUALITY;
    cfg.fb_count = XIAOZHI_CAM_FB_COUNT;
    cfg.fb_location = CAMERA_FB_IN_PSRAM;
    cfg.grab_mode = CAMERA_GRAB_LATEST;
    cfg.jpeg_buffer_size = 0; /* 0 = 使用默认大小 */

    /* 选 DMA 缓冲位置：内部 DMA 需要 16KB 连续内部 RAM，监控模式进来时往往已经不够 */
    xiaozhi_camera_log_heap("before init");
    if (heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) < 4096) {
        /* 驱动建 cam_task 时用的是 FreeRTOS 栈（内部 RAM 专属）且不检查返回值，
         * 内部连续块不足会表现为"初始化成功但一直 grab failed" */
        ESP_LOGW(TAG, "internal RAM too tight: largest free block < 4 KB, cam_task 栈可能申请失败");
    }
#if XIAOZHI_CAM_DMA_MODE == 1
    bool psram_dma = true;
    ESP_LOGI(TAG, "dma: forced PSRAM");
#elif XIAOZHI_CAM_DMA_MODE == 2
    bool psram_dma = false;
    ESP_LOGI(TAG, "dma: forced internal");
#else
    size_t internal_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    bool psram_dma = (internal_largest < XIAOZHI_CAM_INTERNAL_DMA_MIN);
    ESP_LOGI(TAG, "dma: auto -> %s (internal largest block %u B, need %u B)", psram_dma ? "PSRAM" : "internal", (unsigned)internal_largest, (unsigned)XIAOZHI_CAM_INTERNAL_DMA_MIN);
#endif

    esp_err_t err = xiaozhi_camera_init_once(&cfg, psram_dma);
    if (err != ESP_OK) {
        /* 内部 DMA 因内存不足失败时改 PSRAM DMA，或反之，各重试一次 */
        ESP_LOGW(TAG, "init failed with %s DMA (%s), retry with %s DMA", psram_dma ? "PSRAM" : "internal", esp_err_to_name(err), psram_dma ? "internal" : "PSRAM");
        xiaozhi_camera_log_heap("before retry");
        err = xiaozhi_camera_init_once(&cfg, !psram_dma);
        if (err == ESP_OK) {
            psram_dma = !psram_dma;
        }
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_camera_init failed: %s (SCCB 总线是否已由 xiaozhi_audio_init 创建? 内部 RAM 是否够?)", esp_err_to_name(err));
        return err;
    }

    sensor_t *sensor = esp_camera_sensor_get();
    if (sensor != NULL) {
        /* 保持中性画质，避免与云端/前端期望不一致 */
        sensor->set_brightness(sensor, 0);
        sensor->set_contrast(sensor, 0);
        sensor->set_saturation(sensor, 0);
        sensor->set_special_effect(sensor, 0);
        ESP_LOGI(TAG, "sensor id: PID=0x%02x VER=0x%02x MIDH=0x%02x MIDL=0x%02x", sensor->id.PID, sensor->id.VER, sensor->id.MIDH, sensor->id.MIDL);
    }

    ESP_LOGI(TAG, "camera ready: QVGA %s, fb=%d, xclk=%d MHz, sccb=I2C%d(shared with ES8311), dma=%s", (fmt == XIAOZHI_CAM_FMT_RGB565) ? "RGB565" : "JPEG", (int)cfg.fb_count,
             XIAOZHI_CAM_XCLK_HZ / 1000000, XIAOZHI_CAM_SCCB_I2C_PORT, esp_camera_get_psram_mode() ? "PSRAM" : "internal");
    xiaozhi_camera_log_heap("after init");
    return ESP_OK;
}

static esp_err_t xiaozhi_camera_lock(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    return (xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) ? ESP_OK : ESP_ERR_TIMEOUT;
}

static void xiaozhi_camera_unlock(void)
{
    if (s_lock != NULL) {
        xSemaphoreGive(s_lock);
    }
}

esp_err_t xiaozhi_camera_init(xiaozhi_cam_format_t fmt)
{
    esp_err_t ret = xiaozhi_camera_lock();
    if (ret != ESP_OK) {
        return ret;
    }

    if (s_inited) {
        xiaozhi_camera_unlock();
        return ESP_OK;
    }

    ret = xiaozhi_camera_start(fmt);
    if (ret == ESP_OK) {
        s_inited = true;
        s_fmt = fmt;
        ESP_LOGI(TAG, "psram free after init: %u KB", (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    }

    xiaozhi_camera_unlock();
    return ret;
}

esp_err_t xiaozhi_camera_set_format(xiaozhi_cam_format_t fmt)
{
    if (fmt != XIAOZHI_CAM_FMT_JPEG && fmt != XIAOZHI_CAM_FMT_RGB565) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = xiaozhi_camera_lock();
    if (ret != ESP_OK) {
        return ret;
    }
    if (!s_inited) {
        xiaozhi_camera_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    if (fmt == s_fmt) {
        xiaozhi_camera_unlock();
        return ESP_OK;
    }

    ESP_LOGI(TAG, "switch format: %s -> %s", (s_fmt == XIAOZHI_CAM_FMT_RGB565) ? "RGB565" : "JPEG", (fmt == XIAOZHI_CAM_FMT_RGB565) ? "RGB565" : "JPEG");

    /* 先置未就绪，避免预览任务在 deinit 与重新 init 之间取帧 */
    s_inited = false;
    (void)esp_camera_deinit();
    ret = xiaozhi_camera_start(fmt);
    if (ret == ESP_OK) {
        s_inited = true;
        s_fmt = fmt;
    }

    xiaozhi_camera_unlock();
    return ret;
}

esp_err_t xiaozhi_camera_deinit(void)
{
    esp_err_t ret = xiaozhi_camera_lock();
    if (ret != ESP_OK) {
        return ret;
    }
    if (!s_inited) {
        xiaozhi_camera_unlock();
        return ESP_OK;
    }
    ret = esp_camera_deinit();
    s_inited = false;
    ESP_LOGI(TAG, "camera deinit (%s)", esp_err_to_name(ret));
    xiaozhi_camera_unlock();
    return ret;
}

bool xiaozhi_camera_ready(void) { return s_inited; }

xiaozhi_cam_format_t xiaozhi_camera_format(void) { return s_fmt; }

camera_fb_t *xiaozhi_camera_grab(void)
{
    if (!s_inited) {
        return NULL;
    }
    return esp_camera_fb_get();
}

void xiaozhi_camera_return(camera_fb_t *fb)
{
    if (fb != NULL) {
        esp_camera_fb_return(fb);
    }
}

/* -------------------------------------------------------------------------- */
/* 抓拍落盘                                                                     */
/* -------------------------------------------------------------------------- */

/* 板子无 RTC；SNTP 未接入前用开机时长命名，避免出现 1970 的假时间 */
static bool xiaozhi_camera_time_synced(struct tm *out)
{
    time_t now = time(NULL);
    if (now < 1700000000) { /* 2023-11-14 之前都视为未授时 */
        return false;
    }
    return localtime_r(&now, out) != NULL;
}

static void xiaozhi_camera_make_path(const char *tag, char *dir, size_t dir_len, char *path, size_t path_len)
{
    struct tm tm_now;
    const char *prefix = (tag != NULL && tag[0] != '\0') ? tag : "SNAP";

    if (xiaozhi_camera_time_synced(&tm_now)) {
        char day[16];
        char ts[24];
        strftime(day, sizeof(day), "%Y-%m-%d", &tm_now);
        strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", &tm_now);
        snprintf(dir, dir_len, "%s/%s", XIAOZHI_CAM_SNAPSHOT_DIR, day);
        snprintf(path, path_len, "%s/%s_%s_%03u.jpg", dir, prefix, ts, (unsigned)(s_snapshot_seq % 1000));
    } else {
        snprintf(dir, dir_len, "%s/BOOT", XIAOZHI_CAM_SNAPSHOT_DIR);
        snprintf(path, path_len, "%s/%s_BOOT_%lu_%03u.jpg", dir, prefix, (unsigned long)(esp_timer_get_time() / 1000000), (unsigned)(s_snapshot_seq % 1000));
    }
    s_snapshot_seq++;
}

esp_err_t xiaozhi_camera_snapshot(const char *tag)
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }

    /* 抓拍必须是 JPEG；当前是 RGB565 时临时切过去，写完再切回来 */
    xiaozhi_cam_format_t prev_fmt = s_fmt;
    bool switched = false;
    if (prev_fmt != XIAOZHI_CAM_FMT_JPEG) {
        esp_err_t err = xiaozhi_camera_set_format(XIAOZHI_CAM_FMT_JPEG);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "snapshot: 切到 JPEG 失败，跳过 (RGB565 模式无法直接存 JPEG)");
            return err;
        }
        switched = true;
    }

    camera_fb_t *fb = esp_camera_fb_get();
    if (fb == NULL) {
        ESP_LOGW(TAG, "snapshot: grab failed");
        if (switched) {
            (void)xiaozhi_camera_set_format(prev_fmt);
        }
        return ESP_FAIL;
    }

    esp_err_t ret = ESP_OK;
    if (fb->format != PIXFORMAT_JPEG) {
        ESP_LOGW(TAG, "snapshot: frame is not JPEG (fmt=%d), skipped", (int)fb->format);
        ret = ESP_ERR_NOT_SUPPORTED;
    } else if (!xiaozhi_sdcard_ready()) {
        ESP_LOGW(TAG, "snapshot: SD 未挂载，跳过写盘（%u bytes 丢弃）", (unsigned)fb->len);
        ret = ESP_ERR_INVALID_STATE;
    } else {
        char dir[96];
        char path[160];
        xiaozhi_camera_make_path(tag, dir, sizeof(dir), path, sizeof(path));
        (void)xiaozhi_sdcard_ensure_dir(dir);

        FILE *fp = fopen(path, "wb");
        if (fp == NULL) {
            ESP_LOGE(TAG, "snapshot: open %s failed", path);
            ret = ESP_FAIL;
        } else {
            size_t written = fwrite(fb->buf, 1, fb->len, fp);
            fclose(fp);
            if (written != fb->len) {
                ESP_LOGE(TAG, "snapshot: write %s short (%u/%u)", path, (unsigned)written, (unsigned)fb->len);
                ret = ESP_FAIL;
            } else {
                ESP_LOGI(TAG, "snapshot saved: %s (%ux%u, %u bytes)", path, (unsigned)fb->width, (unsigned)fb->height, (unsigned)fb->len);
            }
        }
    }

    esp_camera_fb_return(fb);

    if (switched) {
        (void)xiaozhi_camera_set_format(prev_fmt);
    }
    return ret;
}

int xiaozhi_camera_boot_snapshots(int count)
{
    if (count <= 0) {
        return 0;
    }
    if (!s_inited) {
        ESP_LOGW(TAG, "boot snapshots skipped: camera not ready");
        return 0;
    }

    int ok = 0;
    for (int i = 0; i < count; i++) {
        if (xiaozhi_camera_snapshot("SNAP") == ESP_OK) {
            ok++;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    ESP_LOGI(TAG, "boot snapshots: %d/%d saved to SD", ok, count);
    return ok;
}

#else /* XIAOZHI_CAM_ENABLE == 0：全部空实现，不占用 DVP/PSRAM */

esp_err_t xiaozhi_camera_init(xiaozhi_cam_format_t fmt)
{
    (void)fmt;
    return ESP_OK;
}
esp_err_t xiaozhi_camera_set_format(xiaozhi_cam_format_t fmt)
{
    (void)fmt;
    return ESP_OK;
}
esp_err_t xiaozhi_camera_deinit(void) { return ESP_OK; }
bool xiaozhi_camera_ready(void) { return false; }
xiaozhi_cam_format_t xiaozhi_camera_format(void) { return XIAOZHI_CAM_FMT_JPEG; }
camera_fb_t *xiaozhi_camera_grab(void) { return NULL; }
void xiaozhi_camera_return(camera_fb_t *fb) { (void)fb; }
esp_err_t xiaozhi_camera_snapshot(const char *tag)
{
    (void)tag;
    return ESP_ERR_NOT_SUPPORTED;
}
int xiaozhi_camera_boot_snapshots(int count)
{
    (void)count;
    return 0;
}

#endif /* XIAOZHI_CAM_ENABLE */
