#include "xiaozhi_lvgl.h"

#include <stdio.h>
#include <string.h>

#include "xiaozhi_http.h"
#include "xiaozhi_sdcard.h"

#include "esp_flash.h"
#include "esp_heap_caps.h"

/* 默认显示界面 */
void xiaozhi_lvgl_show_init(void);

#define TAG "xiaozhi_lvgl"
#define LCD_H_RES (320)
#define LCD_V_RES (240)
#define LCD_DRAW_BUFF_HEIGHT 16

extern esp_lcd_panel_handle_t panel_handle;
extern esp_lcd_panel_io_handle_t io_handle;

extern const lv_font_t font_puhui_14_1;
extern const lv_font_t font_puhui_16_4;
extern const lv_font_t font_puhui_20_4;
/* LVGL 内置字体（sdkconfig 中启用）：montserrat_14 含 LV_SYMBOL_WIFI 图标，
 * montserrat_12 用作时间标签的“小字号”（只有数字/符号，无需中文字形） */
extern const lv_font_t lv_font_montserrat_14;
extern const lv_font_t lv_font_montserrat_12;

static lv_display_t *lvgl_disp = NULL;

lv_obj_t *qrcode = NULL;
lv_obj_t *screen = NULL;

/* 公用屏幕对象：全项目唯一入口，避免各模块各自 lv_screen_active() */
lv_obj_t *xiaozhi_lvgl_screen(void)
{
    if (screen == NULL) {
        screen = lv_screen_active();
    }
    return screen;
}

/* -------------------------------------------------------------------------- */
/* 底部推流地址条（SW3 单击显示 / 再点隐藏）                                     */
/*   1. 位置固定在屏幕最下一行（宽 320、高 20），半透明深色底 + 白字；           */
/*   2. 显示时调 lv_obj_move_foreground() 置顶：标题、emoji、文本、监控全屏画面  */
/*      之上都能盖住；                                                            */
/*   3. 监控画面每次 open/rebind 都会把自己置顶，所以视频视图那边也补了一次       */
/*      “把地址条重新提上来”（见 xiaozhi_lvgl_video_apply_style）。              */
/* -------------------------------------------------------------------------- */
#define XIAOZHI_LVGL_URL_BAR_H      20
#define XIAOZHI_LVGL_URL_BAR_BG     0x202020
#define XIAOZHI_LVGL_URL_BAR_FG     0xFFFFFF

static lv_obj_t *s_url_bar = NULL;   /* 底条容器（懒创建，创建后常驻并隐藏） */
static lv_obj_t *s_url_label = NULL; /* 底条里的地址文本 */
static bool s_url_visible = false;   /* 当前是否显示 */

/* -------------------------------------------------------------------------- */
/* 覆盖层：OTA 升级进度面板 + 设备详情页（SW2 双击打开）                          */
/*   层级（自下而上）：普通界面 → 监控全屏画面 → 升级面板 → 推流地址条 → 详情页  */
/*   详情页是模态页面，显示时永远最上层（盖住地址条与升级面板）。                */
/* -------------------------------------------------------------------------- */
static lv_obj_t *s_ota_panel = NULL;  /* 升级进度面板（懒创建） */
static lv_obj_t *s_ota_bar = NULL;    /* 面板里的进度条 */
static lv_obj_t *s_ota_text = NULL;   /* 面板里的文字 */
static bool s_ota_visible = false;

/* 设备详情页（4 行：版本 / Flash / PSRAM / SD） */
static lv_obj_t *s_detail_panel = NULL;
static lv_obj_t *s_detail_val[4] = {NULL, NULL, NULL, NULL};
static bool s_detail_open = false;
static char s_fw_version[16] = "V--"; /* 由 xiaozhi_lvgl_set_version() 写入 */

#define XIAOZHI_LVGL_OTA_PANEL_W 250
#define XIAOZHI_LVGL_OTA_PANEL_H 56

/* 地址条显示中时把它提回最上层；详情页若打开则排它上面（调用前须已持 LVGL 锁） */
static void xiaozhi_lvgl_url_raise(void)
{
    if (s_url_visible && s_url_bar != NULL) {
        lv_obj_move_foreground(s_url_bar);
    }
    if (s_detail_open && s_detail_panel != NULL) {
        lv_obj_move_foreground(s_detail_panel);
    }
}

/* 把"覆盖在画面之上"的几个对象按层级重新提上来（调用前须已持 LVGL 锁） */
static void xiaozhi_lvgl_overlay_raise(void)
{
    if (s_ota_visible && s_ota_panel != NULL) {
        lv_obj_move_foreground(s_ota_panel);
    }
    xiaozhi_lvgl_url_raise();
}

/* -------------------------------------------------------------------------- */
/* 视频视图：把 LVGL 图像对象的细节全部收在本文件                                */
/*   业务侧（camview）只提供一块 RGB565 像素缓冲 + 尺寸，不再出现 lv_* 调用。   */
/* -------------------------------------------------------------------------- */

#define XIAOZHI_LVGL_VIDEO_BORDER_COLOR 0x303030

static lv_obj_t *s_video_img = NULL;
static lv_image_dsc_t s_video_dsc;
static int s_video_w = 0;
static int s_video_h = 0;
static bool s_video_fullscreen = false;
static int s_video_offset_x = 0;
static int s_video_offset_y = 0;
static bool s_video_visible = false;

/* 用外部缓冲填 lv_image_dsc_t（不拷贝像素） */
static void xiaozhi_lvgl_video_fill_dsc(const void *rgb565, int w, int h)
{
    memset(&s_video_dsc, 0, sizeof(s_video_dsc));
    s_video_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_video_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    s_video_dsc.header.w = w;
    s_video_dsc.header.h = h;
    s_video_dsc.header.stride = w * (int)sizeof(uint16_t);
    s_video_dsc.data_size = (uint32_t)((size_t)w * (size_t)h * sizeof(uint16_t));
    s_video_dsc.data = (const uint8_t *)rgb565;
}

/* 尺寸/对齐/边框/层级（调用方必须已持 LVGL 端口锁） */
static void xiaozhi_lvgl_video_apply_style(void)
{
    lv_obj_set_size(s_video_img, s_video_w, s_video_h);
    if (s_video_fullscreen) {
        /* 全屏：铺满整个屏幕、无边框、置顶（标题/emoji/文本被画面覆盖） */
        lv_obj_align(s_video_img, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_style_border_width(s_video_img, 0, 0);
    } else {
        lv_obj_align(s_video_img, LV_ALIGN_TOP_RIGHT, -s_video_offset_x, s_video_offset_y);
        lv_obj_set_style_border_width(s_video_img, 1, 0);
        lv_obj_set_style_border_color(s_video_img, lv_color_hex(XIAOZHI_LVGL_VIDEO_BORDER_COLOR), 0);
    }
    lv_obj_set_scrollable(s_video_img, false);
    lv_obj_set_style_radius(s_video_img, 0, 0);
    lv_obj_set_style_pad_all(s_video_img, 0, 0);
    lv_obj_set_hidden(s_video_img, !s_video_visible);
    lv_obj_move_foreground(s_video_img);
    /* 画面置顶后要把升级面板/底部地址条/设备详情页再提回来，否则全屏预览会盖住它们 */
    xiaozhi_lvgl_overlay_raise();
}

bool xiaozhi_lvgl_video_is_open(void)
{
    return s_video_img != NULL;
}

bool xiaozhi_lvgl_video_is_visible(void)
{
    return s_video_visible;
}

esp_err_t xiaozhi_lvgl_video_open(const xiaozhi_lvgl_video_cfg_t *cfg)
{
    if (cfg == NULL || cfg->rgb565 == NULL || cfg->w <= 0 || cfg->h <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 形态变化（全屏 ↔ 叠加）必须重建对象，尺寸变化 rebind 即可 */
    if (s_video_img != NULL && cfg->fullscreen != s_video_fullscreen) {
        esp_err_t err = xiaozhi_lvgl_video_close();
        if (err != ESP_OK) {
            return err;
        }
    }

    s_video_w = cfg->w;
    s_video_h = cfg->h;
    s_video_fullscreen = cfg->fullscreen;
    s_video_offset_x = cfg->offset_x;
    s_video_offset_y = cfg->offset_y;
    xiaozhi_lvgl_video_fill_dsc(cfg->rgb565, cfg->w, cfg->h);

    if (!lvgl_port_lock(1000)) {
        return ESP_ERR_TIMEOUT;
    }
    if (s_video_img == NULL) {
        lv_obj_t *scr = xiaozhi_lvgl_screen();
        if (scr == NULL) {
            lvgl_port_unlock();
            return ESP_ERR_INVALID_STATE;
        }
        s_video_img = lv_image_create(scr);
        if (s_video_img == NULL) {
            lvgl_port_unlock();
            return ESP_ERR_NO_MEM;
        }
        s_video_visible = true;
    }
    lv_image_set_src(s_video_img, &s_video_dsc);
    xiaozhi_lvgl_video_apply_style();
    lv_obj_invalidate(s_video_img);
    lvgl_port_unlock();
    return ESP_OK;
}

esp_err_t xiaozhi_lvgl_video_rebind(const void *rgb565, int w, int h)
{
    if (s_video_img == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (rgb565 == NULL || w <= 0 || h <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!lvgl_port_lock(500)) {
        return ESP_ERR_TIMEOUT;
    }
    s_video_w = w;
    s_video_h = h;
    xiaozhi_lvgl_video_fill_dsc(rgb565, w, h);
    lv_image_set_src(s_video_img, &s_video_dsc);
    xiaozhi_lvgl_video_apply_style();
    lv_obj_invalidate(s_video_img);
    lvgl_port_unlock();
    return ESP_OK;
}

esp_err_t xiaozhi_lvgl_video_close(void)
{
    if (s_video_img == NULL) {
        s_video_visible = false;
        return ESP_OK;
    }
    if (!lvgl_port_lock(500)) {
        return ESP_ERR_TIMEOUT;
    }
    lv_obj_delete(s_video_img);
    s_video_img = NULL;
    s_video_visible = false;
    lvgl_port_unlock();
    return ESP_OK;
}

esp_err_t xiaozhi_lvgl_video_update(void (*paint)(void *ctx), void *ctx, uint32_t timeout_ms)
{
    if (s_video_img == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!lvgl_port_lock(timeout_ms)) {
        return ESP_ERR_TIMEOUT;
    }
    /* 等锁期间视图可能已被关闭，重新确认一次 */
    if (s_video_img == NULL) {
        lvgl_port_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    if (paint != NULL) {
        paint(ctx);
    }
    lv_obj_invalidate(s_video_img);
    lvgl_port_unlock();
    return ESP_OK;
}

void xiaozhi_lvgl_video_set_visible(bool visible)
{
    s_video_visible = visible;
    if (s_video_img == NULL) {
        return;
    }
    if (!lvgl_port_lock(100)) {
        return;
    }
    lv_obj_set_hidden(s_video_img, !visible);
    if (visible) {
        lv_obj_move_foreground(s_video_img);
        /* 画面重新显示时同样把覆盖层（版本号/升级面板/地址条）提回最上层 */
        xiaozhi_lvgl_overlay_raise();
        lv_obj_invalidate(s_video_img);
    }
    lvgl_port_unlock();
}

lv_obj_t *title_label = NULL;
lv_obj_t *emoji_label = NULL;
lv_obj_t *text_label = NULL;
/* 标题栏右侧的北京时间（"YYYY-MM-DD HH:MM:SS"），未授时显示 "--" */
lv_obj_t *datetime_label = NULL;
/* 标题栏最右侧的 WiFi 状态图标（不显示文字，靠颜色区分连接中/已连接/失败） */
lv_obj_t *wifi_label = NULL;

/* 标题栏右侧布局：WiFi 图标固定占位宽度（14px 字体下 wifi 图标约 14px），
 * 宽度写死可让左侧时间标签的位置固定，状态切换时不会跳动 */
#define XIAOZHI_LVGL_WIFI_ICON_W 18
#define XIAOZHI_LVGL_TITLE_PAD   4

/* WiFi 状态的缓存：wifi 事件可能早于界面创建（或界面重建），先记住状态 */
static xiaozhi_wifi_state_t s_wifi_state = XIAOZHI_WIFI_STATE_CONNECTING;

typedef struct {
    char *name;
    char *emoji;
} emoji_list_t;

const emoji_list_t emoji_list[21] = {{.name = "neutral", .emoji = "😶"},   {.name = "happy", .emoji = "🙂"},   {.name = "laughing", .emoji = "😆"},
                                     {.name = "funny", .emoji = "😂"},     {.name = "sad", .emoji = "😔"},     {.name = "angry", .emoji = "😠"},
                                     {.name = "crying", .emoji = "😭"},    {.name = "loving", .emoji = "😍"},  {.name = "embarrassed", .emoji = "😳"},
                                     {.name = "surprised", .emoji = "😲"}, {.name = "shocked", .emoji = "😱"}, {.name = "thinking", .emoji = "🤔"},
                                     {.name = "winking", .emoji = "😉"},   {.name = "cool", .emoji = "😎"},    {.name = "relaxed", .emoji = "😌"},
                                     {.name = "delicious", .emoji = "🤤"}, {.name = "kissy", .emoji = "😘"},   {.name = "confident", .emoji = "😏"},
                                     {.name = "sleepy", .emoji = "😴"},    {.name = "silly", .emoji = "😜"},   {.name = "confused", .emoji = "🙄"}};
void xiaozhi_lvgl_init(void)
{
    // LCD初始化
    xiaozhi_lcd_init();
    // LVGL初始化
    const lvgl_port_cfg_t lvgl_cfg = {
        .task_priority = 4,       /* LVGL task priority */
        .task_stack = 8192,       /* LVGL task stack size */
        .task_affinity = -1,      /* LVGL task pinned to core (-1 is no affinity) */
        .task_max_sleep_ms = 500, /* Maximum sleep in LVGL task */
        .timer_period_ms = 5      /* LVGL timer tick period in ms */
    };

    lvgl_port_init(&lvgl_cfg);

    ESP_LOGI(TAG, "Add LCD screen");
    const lvgl_port_display_cfg_t disp_cfg = {.io_handle = io_handle,
                                              .panel_handle = panel_handle,
                                              /* buffer_size 的单位是"像素"，esp_lvgl_port 内部会再乘
                                               * color_bytes(2) 得到字节数。原写法多乘了一次 2，
                                               * 实际每块 = 320*16*2*2 = 20480B，双缓冲白占 40KB
                                               * 内部 DMA RAM；修正后每块 10240B、双缓冲共 20KB，
                                               * 为监控模式的相机（DMA 缓冲/cam_task 栈）腾出 20KB。 */
                                              .buffer_size = LCD_H_RES * LCD_DRAW_BUFF_HEIGHT,
                                              .double_buffer = true,
                                              .hres = LCD_H_RES,
                                              .vres = LCD_V_RES,
                                              .monochrome = false, // 是否使用单通道渲染
                                              .color_format = LV_COLOR_FORMAT_RGB565,
                                              .rotation =
                                                  {
                                                      .swap_xy = true,
                                                      .mirror_x = true,
                                                      .mirror_y = false,
                                                  },
                                              .flags = {
                                                  .buff_dma = true,
                                                  .swap_bytes = true,
                                              }};

    lvgl_disp = lvgl_port_add_disp(&disp_cfg);

    // 全局的默认样式
    // 上锁
    lvgl_port_lock(0);
    screen = lv_screen_active();
    // 设置屏幕背景颜色
    lv_obj_set_style_bg_color(screen, lv_color_hex(0xffffff), LV_PART_MAIN);
    // 设置文本默认颜色
    lv_obj_set_style_text_color(screen, lv_color_hex(0x000000), LV_PART_MAIN);
    // 解锁
    lvgl_port_unlock();

    xiaozhi_lvgl_show_init();
}

/* 显示hello world */
void xiaozhi_lvgl_show_hello_world(void)
{
    // ! lvgl操作屏幕对象是 非原子性
    // 上锁
    lvgl_port_lock(0);
    // 获取屏幕对象（公用入口，不再各自 lv_screen_active()）
    lv_obj_t *scr = xiaozhi_lvgl_screen();
    if (scr == NULL) {
        lvgl_port_unlock();
        return;
    }

    lv_obj_set_style_bg_color(scr, lv_color_hex(0x003a57), 0);
    lv_obj_set_style_text_color(scr, lv_color_hex(0xffffff), 0);
    // 创建label标签，用于显示文本内容
    lv_obj_t *label = lv_label_create(scr);
    // 设置对齐方式  set_align相对于父元素对齐
    lv_obj_set_align(label, LV_ALIGN_CENTER);
    lv_label_set_text(label, "Hello world");
    // 解锁
    lvgl_port_unlock();
}

/* 显示二维码 */
void xiaozhi_lvgl_show_qrcode(char *qrcode_data)
{
    // 上锁
    lvgl_port_lock(0);
    lv_obj_t *scr = xiaozhi_lvgl_screen();
    if (scr == NULL) {
        lvgl_port_unlock();
        return;
    }
    // 创建一个qrcode对象
    qrcode = lv_qrcode_create(scr);
    // 设置居中
    lv_obj_center(qrcode);
    // 设置大小
    lv_qrcode_set_size(qrcode, 150);
    lv_qrcode_set_dark_color(qrcode, lv_color_hex(0x000000));
    lv_qrcode_set_light_color(qrcode, lv_color_hex(0xffffff));
    // 设置二维码的数据
    lv_qrcode_set_data(qrcode, qrcode_data);
    // 给二维码周边设置护城河
    lv_qrcode_set_quiet_zone(qrcode, true);
    // 新建的对象默认在最上层，把覆盖层重新提回来（设备详情页 > 地址条 > 升级面板）
    xiaozhi_lvgl_overlay_raise();
    // 解锁
    lvgl_port_unlock();
}

/* 删除二维码 */
void xiaozhi_lvgl_delete_qrcode(void)
{
    if (qrcode == NULL) {
        return;
    }
    // 上锁
    lvgl_port_lock(0);
    lv_obj_delete(qrcode);
    // 解锁
    lvgl_port_unlock();
}

/* 按缓存的状态刷新 WiFi 图标的颜色（调用前须已持有 LVGL 锁）
 * 只显示 wifi 小图标、不再显示文字，状态用颜色区分：
 * 灰=连接中/未知，绿=已连接，红=失败/断开 */
static void xiaozhi_lvgl_wifi_apply(void)
{
    if (wifi_label == NULL) {
        return;
    }
    uint32_t color = 0x8A8A8A; /* 灰：连接中/未知 */
    if (s_wifi_state == XIAOZHI_WIFI_STATE_CONNECTED) {
        color = 0x1BA034; /* 绿：连接成功 */
    } else if (s_wifi_state == XIAOZHI_WIFI_STATE_FAILED) {
        color = 0xE53935; /* 红：连接失败/断开 */
    }
    lv_label_set_text(wifi_label, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_color(wifi_label, lv_color_hex(color), 0);
}

/* 默认显示界面 */
void xiaozhi_lvgl_show_init(void)
{
    // 上锁
    lvgl_port_lock(0);
    // ! 标题区域
    // 创建标题区域容器
    lv_obj_t *title_container = lv_obj_create(screen);
    // 设置圆角
    lv_obj_set_style_radius(title_container, 0, LV_PART_MAIN);
    // 设置滚动条
    lv_obj_set_scrollbar_mode(title_container, LV_SCROLLBAR_MODE_OFF);
    // 设置边框
    lv_obj_set_style_border_width(title_container, 0, LV_PART_MAIN);
    // 设置大小
    lv_obj_set_size(title_container, LCD_H_RES, 30);
    // 设置背景颜色
    lv_obj_set_style_bg_color(title_container, lv_color_hex(0xBFCADB), 0);
    // 创建label标签
    title_label = lv_label_create(title_container);
    // 设置字体
    lv_obj_set_style_text_font(title_label, &font_puhui_14_1, 0);
    // 设置label的文本（默认进入语音模式，标题与 appmode 的语音模式标题一致）
    lv_label_set_text(title_label, "AI小智｜语音");
    // 标题靠左，时间/WiFi 标识靠右，共用 320x30 的标题条，互不遮挡
    lv_obj_align(title_label, LV_ALIGN_LEFT_MID, 6, 0);

    // ! 标题栏最右侧的 WiFi 状态图标（时间标签排在它左边）
    wifi_label = lv_label_create(title_container);
    // montserrat_14 内含 LV_SYMBOL_WIFI 图标字形，只显示图标不显示文字
    lv_obj_set_style_text_font(wifi_label, &lv_font_montserrat_14, 0);
    // 固定宽度 + 右对齐，状态切换时图标位置不跳动，也不换行溢出标题条
    lv_obj_set_width(wifi_label, XIAOZHI_LVGL_WIFI_ICON_W);
    lv_obj_set_style_text_align(wifi_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(wifi_label, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_align(wifi_label, LV_ALIGN_RIGHT_MID, -XIAOZHI_LVGL_TITLE_PAD, 0);

    // ! 标题栏右侧的北京时间（由 xiaozhi_time 每秒刷新，未授时显示 "--"）
    datetime_label = lv_label_create(title_container);
    // 时间用更小的 12px 字体（纯数字/符号，不需要中文字形），给标题留出空间
    lv_obj_set_style_text_font(datetime_label, &lv_font_montserrat_12, 0);
    lv_label_set_text(datetime_label, "--");
    // 排在 WiFi 图标左侧，两者之间留 XIAOZHI_LVGL_TITLE_PAD 的间隔
    lv_obj_align(datetime_label, LV_ALIGN_RIGHT_MID,
                 -(XIAOZHI_LVGL_TITLE_PAD * 2 + XIAOZHI_LVGL_WIFI_ICON_W), 0);

    // 按缓存状态先渲染一次（配网/WiFi 事件可能早于界面创建完成）
    xiaozhi_lvgl_wifi_apply();

    // ! emoji区域
    // 创建label标签
    emoji_label = lv_label_create(screen);
    // 设置宽度
    lv_obj_set_width(emoji_label, 320);
    // 设置文本居中
    lv_obj_set_style_text_align(emoji_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(emoji_label, font_emoji_64_init(), 0);
    lv_label_set_text(emoji_label, "🙂");
    // 设置相对对齐方式
    lv_obj_align_to(emoji_label, title_container, LV_ALIGN_OUT_BOTTOM_MID, 0, 30);
    // ! 文本区域
    // 创建label标签
    text_label = lv_label_create(screen);
    // 设置宽度
    lv_obj_set_width(text_label, 280);
    // 设置文本居中
    lv_obj_set_style_text_align(text_label, LV_TEXT_ALIGN_CENTER, 0);
    // 设置字体
    lv_obj_set_style_text_font(text_label, &font_puhui_14_1, 0);
    lv_label_set_text(text_label, "你好，我是小智");
    // 设置相对对齐方式
    lv_obj_align_to(text_label, emoji_label, LV_ALIGN_OUT_BOTTOM_MID, 0, 30);

    // ! 首页不再显示版本号：版本等信息改到 SW2 双击打开的“设备详情页”里查看
    // 解锁
    lvgl_port_unlock();
}

/* -------------------------------------------------------------------------- */
/* 设备详情页（SW2 双击打开/关闭，模态，永远最上层）                             */
/*   4 行：版本 / Flash / PSRAM / SD；内容每次打开时重新采集。                    */
/* -------------------------------------------------------------------------- */
#define XIAOZHI_LVGL_DETAIL_W         280
#define XIAOZHI_LVGL_DETAIL_H         150
#define XIAOZHI_LVGL_DETAIL_BG        0x181818
#define XIAOZHI_LVGL_DETAIL_BORDER    0x4A4A4A
#define XIAOZHI_LVGL_DETAIL_FG        0xFFFFFF
#define XIAOZHI_LVGL_DETAIL_SUB       0xA0A0A0
#define XIAOZHI_LVGL_DETAIL_PAD       10
#define XIAOZHI_LVGL_DETAIL_ROW_Y0    26
#define XIAOZHI_LVGL_DETAIL_ROW_H     24
#define XIAOZHI_LVGL_DETAIL_LABEL_W   56

static const char *const s_detail_name[4] = {"版本", "Flash", "PSRAM", "SD"};

/* 字节数 → "x.x MB" / "x.x GB"（整数 tenths 计算，避免浮点 printf 触发
 * -Wformat-truncation 告警——本项目按错误处理） */
static void xiaozhi_lvgl_detail_fmt_size(uint64_t bytes, char *buf, size_t len)
{
    const uint64_t mb = 1024ULL * 1024ULL;
    const uint64_t gb = 1024ULL * 1024ULL * 1024ULL;
    if (bytes >= gb) {
        uint64_t tenth = (bytes * 10ULL + gb / 2ULL) / gb;
        snprintf(buf, len, "%llu.%llu GB",
                 (unsigned long long)(tenth / 10ULL), (unsigned long long)(tenth % 10ULL));
    } else {
        uint64_t tenth = (bytes * 10ULL + mb / 2ULL) / mb;
        snprintf(buf, len, "%llu.%llu MB",
                 (unsigned long long)(tenth / 10ULL), (unsigned long long)(tenth % 10ULL));
    }
}

/* 拼接 "a + sep + b"（长度编译期未知时手工拼，避免 -Wformat-truncation） */
static void xiaozhi_lvgl_detail_join(char *dst, size_t dst_len,
                                    const char *a, const char *sep, const char *b)
{
    size_t na = (a != NULL) ? strlen(a) : 0U;
    size_t ns = (sep != NULL) ? strlen(sep) : 0U;
    size_t nb = (b != NULL) ? strlen(b) : 0U;
    if (na + ns + nb > dst_len - 1U) {
        nb = (dst_len - 1U > na + ns) ? (dst_len - 1U - na - ns) : 0U;
    }
    if (na > 0U) {
        memcpy(dst, a, na);
    }
    if (ns > 0U) {
        memcpy(dst + na, sep, ns);
    }
    if (nb > 0U) {
        memcpy(dst + na + ns, b, nb);
    }
    dst[na + ns + nb] = '\0';
}

/* 创建详情页（懒创建；调用前须已持 LVGL 锁） */
static void xiaozhi_lvgl_detail_create(void)
{
    if (s_detail_panel != NULL) {
        return;
    }
    lv_obj_t *scr = xiaozhi_lvgl_screen();
    if (scr == NULL) {
        return;
    }
    s_detail_panel = lv_obj_create(scr);
    if (s_detail_panel == NULL) {
        return;
    }
    lv_obj_set_style_radius(s_detail_panel, 8, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_detail_panel, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_detail_panel, lv_color_hex(XIAOZHI_LVGL_DETAIL_BORDER), LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_detail_panel, XIAOZHI_LVGL_DETAIL_PAD, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(s_detail_panel, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scrollable(s_detail_panel, false);
    /* 模态页：不透明底，盖住下面的标题/emoji/文本/监控画面 */
    lv_obj_set_style_bg_color(s_detail_panel, lv_color_hex(XIAOZHI_LVGL_DETAIL_BG), 0);
    lv_obj_set_style_bg_opa(s_detail_panel, LV_OPA_COVER, 0);
    lv_obj_set_size(s_detail_panel, XIAOZHI_LVGL_DETAIL_W, XIAOZHI_LVGL_DETAIL_H);
    lv_obj_align(s_detail_panel, LV_ALIGN_CENTER, 0, 0);

    /* 左上标题 + 右上关闭提示 */
    lv_obj_t *title = lv_label_create(s_detail_panel);
    lv_obj_set_style_text_font(title, &font_puhui_14_1, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(XIAOZHI_LVGL_DETAIL_FG), 0);
    lv_label_set_text(title, "设备详情");
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *hint = lv_label_create(s_detail_panel);
    lv_obj_set_style_text_font(hint, &font_puhui_14_1, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(XIAOZHI_LVGL_DETAIL_SUB), 0);
    lv_label_set_text(hint, "SW2双击关闭");
    lv_obj_align(hint, LV_ALIGN_TOP_RIGHT, 0, 0);

    /* 4 行：左侧字段名、右侧数值（数值右对齐，宽度固定不换行） */
    for (int i = 0; i < 4; i++) {
        lv_obj_t *name = lv_label_create(s_detail_panel);
        lv_obj_set_style_text_font(name, &font_puhui_14_1, 0);
        lv_obj_set_style_text_color(name, lv_color_hex(XIAOZHI_LVGL_DETAIL_SUB), 0);
        lv_obj_set_width(name, XIAOZHI_LVGL_DETAIL_LABEL_W);
        lv_label_set_text(name, s_detail_name[i]);
        lv_obj_align(name, LV_ALIGN_TOP_LEFT, 0,
                     XIAOZHI_LVGL_DETAIL_ROW_Y0 + i * XIAOZHI_LVGL_DETAIL_ROW_H);

        s_detail_val[i] = lv_label_create(s_detail_panel);
        lv_obj_set_style_text_font(s_detail_val[i], &font_puhui_14_1, 0);
        lv_obj_set_style_text_color(s_detail_val[i], lv_color_hex(XIAOZHI_LVGL_DETAIL_FG), 0);
        lv_obj_set_width(s_detail_val[i],
                         XIAOZHI_LVGL_DETAIL_W - 2 * XIAOZHI_LVGL_DETAIL_PAD - XIAOZHI_LVGL_DETAIL_LABEL_W);
        lv_obj_set_style_text_align(s_detail_val[i], LV_TEXT_ALIGN_RIGHT, 0);
        lv_label_set_long_mode(s_detail_val[i], LV_LABEL_LONG_MODE_CLIP);
        lv_label_set_text(s_detail_val[i], "--");
        lv_obj_align(s_detail_val[i], LV_ALIGN_TOP_RIGHT, 0,
                     XIAOZHI_LVGL_DETAIL_ROW_Y0 + i * XIAOZHI_LVGL_DETAIL_ROW_H);
    }

    lv_obj_set_hidden(s_detail_panel, true);
}

/* 采集并刷新 4 行数值（调用前须已持 LVGL 锁） */
static void xiaozhi_lvgl_detail_refresh(void)
{
    char buf[96];

    /* 1) 固件版本：由 xiaozhi_lvgl_set_version() 缓存 */
    if (s_detail_val[0] != NULL) {
        lv_label_set_text(s_detail_val[0], s_fw_version);
    }

    /* 2) Flash 总容量 */
    if (s_detail_val[1] != NULL) {
        uint32_t flash_size = 0;
        if (esp_flash_get_size(NULL, &flash_size) != ESP_OK || flash_size == 0U) {
            lv_label_set_text(s_detail_val[1], "未知");
        } else {
            xiaozhi_lvgl_detail_fmt_size(flash_size, buf, sizeof(buf));
            lv_label_set_text(s_detail_val[1], buf);
        }
    }

    /* 3) PSRAM：总容量 / 当前空闲 */
    if (s_detail_val[2] != NULL) {
        size_t ps_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
        size_t ps_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        if (ps_total == 0U) {
            lv_label_set_text(s_detail_val[2], "无");
        } else {
            char t[24];
            char f[24];
            xiaozhi_lvgl_detail_fmt_size((uint64_t)ps_total, t, sizeof(t));
            xiaozhi_lvgl_detail_fmt_size((uint64_t)ps_free, f, sizeof(f));
            xiaozhi_lvgl_detail_join(buf, sizeof(buf), t, " / 空闲 ", f);
            lv_label_set_text(s_detail_val[2], buf);
        }
    }

    /* 4) SD 卡：容量 / 空闲（用缓存值，避免在 UI 线程里阻塞 FATFS） */
    if (s_detail_val[3] != NULL) {
        if (!xiaozhi_sdcard_cache_ready()) {
            lv_label_set_text(s_detail_val[3], "未挂载");
        } else {
            char t[24];
            char f[24];
            xiaozhi_lvgl_detail_fmt_size(xiaozhi_sdcard_total_bytes_cached(), t, sizeof(t));
            xiaozhi_lvgl_detail_fmt_size(xiaozhi_sdcard_free_bytes_cached(), f, sizeof(f));
            xiaozhi_lvgl_detail_join(buf, sizeof(buf), t, " / 空闲 ", f);
            lv_label_set_text(s_detail_val[3], buf);
        }
    }
}

/* 切换设备详情页显示/隐藏（模态：显示时在最上层）；返回切换后的打开状态 */
bool xiaozhi_lvgl_toggle_device_detail(void)
{
    if (!lvgl_port_lock(200)) {
        return s_detail_open;
    }
    if (s_detail_panel == NULL) {
        xiaozhi_lvgl_detail_create();
        if (s_detail_panel == NULL) {
            lvgl_port_unlock();
            ESP_LOGW(TAG, "detail panel create failed (screen not ready)");
            return false;
        }
    }
    s_detail_open = !s_detail_open;
    if (s_detail_open) {
        xiaozhi_lvgl_detail_refresh();
        lv_obj_set_hidden(s_detail_panel, false);
        xiaozhi_lvgl_url_raise(); /* 盖住地址条与升级面板 */
    } else {
        lv_obj_set_hidden(s_detail_panel, true);
    }
    /* 隐藏时也要标脏，让底下的原界面重新画回来 */
    lv_obj_invalidate(s_detail_panel);
    lvgl_port_unlock();
    ESP_LOGI(TAG, "device detail %s", s_detail_open ? "shown" : "hidden");
    return s_detail_open;
}

bool xiaozhi_lvgl_device_detail_is_open(void)
{
    return s_detail_open;
}

/* 记录固件版本（如 "V1.0"），在设备详情页显示；界面未就绪时只缓存
 * 调用方传入的版本号，详情页首次打开时再回填（该函数通常在界面初始化后调用） */
void xiaozhi_lvgl_set_version(const char *ver)
{
    if (ver == NULL || ver[0] == '\0') {
        return;
    }
    /* 手工截断拷贝：版本串长度编译期未知，避免 snprintf 触发 -Wformat-truncation */
    size_t n = strlen(ver);
    if (n > sizeof(s_fw_version) - 1U) {
        n = sizeof(s_fw_version) - 1U;
    }
    memcpy(s_fw_version, ver, n);
    s_fw_version[n] = '\0';

    if (!lvgl_port_lock(200)) {
        ESP_LOGW(TAG, "set_version lock timeout, cached only");
        return;
    }
    if (s_detail_val[0] != NULL) {
        lv_label_set_text(s_detail_val[0], s_fw_version);
        if (s_detail_open) {
            lv_obj_invalidate(s_detail_panel);
        }
    }
    lvgl_port_unlock();
    ESP_LOGI(TAG, "fw version: %s", s_fw_version);
}

/* 创建 OTA 升级进度面板（懒创建；调用前须已持 LVGL 锁） */
static void xiaozhi_lvgl_ota_create(void)
{
    if (s_ota_panel != NULL) {
        return;
    }
    lv_obj_t *scr = xiaozhi_lvgl_screen();
    if (scr == NULL) {
        return;
    }
    s_ota_panel = lv_obj_create(scr);
    if (s_ota_panel == NULL) {
        return;
    }
    lv_obj_set_style_radius(s_ota_panel, 8, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_ota_panel, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_ota_panel, lv_color_hex(0x4A4A4A), LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_ota_panel, 8, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(s_ota_panel, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scrollable(s_ota_panel, false);
    lv_obj_set_style_bg_color(s_ota_panel, lv_color_hex(0x181818), 0);
    lv_obj_set_style_bg_opa(s_ota_panel, LV_OPA_90, 0);
    lv_obj_set_size(s_ota_panel, XIAOZHI_LVGL_OTA_PANEL_W, XIAOZHI_LVGL_OTA_PANEL_H);
    lv_obj_align(s_ota_panel, LV_ALIGN_CENTER, 0, 0);

    s_ota_text = lv_label_create(s_ota_panel);
    lv_obj_set_style_text_font(s_ota_text, &font_puhui_14_1, 0);
    lv_obj_set_style_text_color(s_ota_text, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_width(s_ota_text, XIAOZHI_LVGL_OTA_PANEL_W - 20);
    lv_obj_set_style_text_align(s_ota_text, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_ota_text, "升级中 0%");
    lv_obj_align(s_ota_text, LV_ALIGN_TOP_MID, 0, 0);

    s_ota_bar = lv_bar_create(s_ota_panel);
    lv_obj_set_size(s_ota_bar, XIAOZHI_LVGL_OTA_PANEL_W - 20, 10);
    lv_bar_set_range(s_ota_bar, 0, 100);
    lv_bar_set_value(s_ota_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_ota_bar, lv_color_hex(0x3A3A3A), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_ota_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_ota_bar, lv_color_hex(0x1BA034), LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_ota_bar, 3, LV_PART_MAIN);
    lv_obj_set_style_radius(s_ota_bar, 3, LV_PART_INDICATOR);
    lv_obj_align(s_ota_bar, LV_ALIGN_BOTTOM_MID, 0, 0);

    lv_obj_set_hidden(s_ota_panel, true);
}

/* 显示/更新升级进度（pct 0~100，text 为状态文字，如 "写入中"） */
void xiaozhi_lvgl_ota_progress_show(int pct, const char *text)
{
    if (!lvgl_port_lock(200)) {
        return;
    }
    xiaozhi_lvgl_ota_create();
    if (s_ota_panel != NULL) {
        if (pct < 0) {
            pct = 0;
        }
        if (pct > 100) {
            pct = 100;
        }
        /* 手工拼接：text 长度编译期未知，直接 "%s %d%%" 会触发 -Wformat-truncation（本项目按错误处理） */
        char buf[64];
        const char *label = (text != NULL && text[0] != '\0') ? text : "升级中";
        size_t n = strlen(label);
        if (n > sizeof(buf) - 8) {
            n = sizeof(buf) - 8;
        }
        memcpy(buf, label, n);
        snprintf(buf + n, sizeof(buf) - n, " %d%%", pct);
        lv_label_set_text(s_ota_text, buf);
        lv_bar_set_value(s_ota_bar, pct, LV_ANIM_OFF);
        s_ota_visible = true;
        lv_obj_set_hidden(s_ota_panel, false);
        /* 覆盖标题/文本/监控画面，但地址条仍在最上层 */
        xiaozhi_lvgl_overlay_raise();
        lv_obj_invalidate(s_ota_panel);
    }
    lvgl_port_unlock();
}

/* 收起升级进度面板（失败/取消/重启前） */
void xiaozhi_lvgl_ota_progress_hide(void)
{
    if (!lvgl_port_lock(200)) {
        return;
    }
    s_ota_visible = false;
    if (s_ota_panel != NULL) {
        lv_obj_set_hidden(s_ota_panel, true);
        lv_obj_invalidate(s_ota_panel);
    }
    lvgl_port_unlock();
}

/* 创建底部地址条（懒创建；调用前须已持 LVGL 锁）。
 * 屏幕未就绪（xiaozhi_lvgl_screen() 返回 NULL）时什么都不做，s_url_bar 保持 NULL。 */
static void xiaozhi_lvgl_url_create(void)
{
    lv_obj_t *scr = xiaozhi_lvgl_screen();
    if (scr == NULL) {
        return;
    }
    s_url_bar = lv_obj_create(scr);
    if (s_url_bar == NULL) {
        return;
    }
    /* 纯色底条：去圆角/边框/内边距，固定贴在屏幕最下一行 */
    lv_obj_set_style_radius(s_url_bar, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_url_bar, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_url_bar, 0, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(s_url_bar, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scrollable(s_url_bar, false);
    lv_obj_set_style_bg_color(s_url_bar, lv_color_hex(XIAOZHI_LVGL_URL_BAR_BG), 0);
    lv_obj_set_style_bg_opa(s_url_bar, LV_OPA_80, 0); /* 半透明，监控画面上也看得清 */
    lv_obj_set_size(s_url_bar, LCD_H_RES, XIAOZHI_LVGL_URL_BAR_H);
    lv_obj_align(s_url_bar, LV_ALIGN_BOTTOM_MID, 0, 0);

    s_url_label = lv_label_create(s_url_bar);
    /* puhui_14_1 同时含 ASCII 与中文字形，地址与提示文案都能显示 */
    lv_obj_set_style_text_font(s_url_label, &font_puhui_14_1, 0);
    lv_obj_set_style_text_color(s_url_label, lv_color_hex(XIAOZHI_LVGL_URL_BAR_FG), 0);
    lv_obj_set_width(s_url_label, LCD_H_RES - 12);
    /* 左对齐：地址从左往右铺，长 URL 也不会溢出屏幕 */
    lv_obj_set_style_text_align(s_url_label, LV_TEXT_ALIGN_LEFT, 0);
    lv_label_set_long_mode(s_url_label, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_align(s_url_label, LV_ALIGN_LEFT_MID, 6, 0);
    lv_label_set_text(s_url_label, "");
    /* 创建后先隐藏，等用户点 SW3 再显示 */
    lv_obj_set_hidden(s_url_bar, true);
}

/* 刷新地址文本：每次都重新取（xiaozhi_http_url 内部会重读 netif），
 * WiFi 重连换 IP 也不会残留旧地址——用户要求"必须是实际地址，不能写死" */
static void xiaozhi_lvgl_url_refresh_text(void)
{
    if (s_url_label == NULL) {
        return;
    }
    const char *url = xiaozhi_http_url();
    /* 手工拼接（url 长度编译期未知，避免 -Wformat-truncation 报错） */
    static const char prefix[] = "上位机连接：";
    const char *tail = (url != NULL && url[0] != '\0') ? url : "等待 WiFi 获取 IP";
    char text[96];
    size_t n = sizeof(prefix) - 1;
    memcpy(text, prefix, n);
    size_t t = strlen(tail);
    if (t > sizeof(text) - 1 - n) {
        t = sizeof(text) - 1 - n;
    }
    memcpy(text + n, tail, t);
    text[n + t] = '\0';
    lv_label_set_text(s_url_label, text);
}

/* 切换底部推流地址条的显示/隐藏；返回切换后的可见状态 */
bool xiaozhi_lvgl_toggle_stream_url(void)
{
    if (!lvgl_port_lock(200)) {
        return s_url_visible;
    }
    if (s_url_bar == NULL) {
        xiaozhi_lvgl_url_create();
        if (s_url_bar == NULL) {
            lvgl_port_unlock();
            ESP_LOGW(TAG, "url bar create failed (screen not ready)");
            return false;
        }
    }
    s_url_visible = !s_url_visible;
    if (s_url_visible) {
        xiaozhi_lvgl_url_refresh_text();
        lv_obj_set_hidden(s_url_bar, false);
        xiaozhi_lvgl_url_raise(); /* 覆盖标题/文本/监控全屏画面 */
    } else {
        lv_obj_set_hidden(s_url_bar, true);
    }
    /* 隐藏时也要标脏，让底下的原界面重新画回来 */
    lv_obj_invalidate(s_url_bar);
    lvgl_port_unlock();
    ESP_LOGI(TAG, "stream url bar %s", s_url_visible ? "shown" : "hidden");
    return s_url_visible;
}

/* 修改标题的方法 */
void xiaozhi_lvgl_set_title(char *title)
{
    // 界面未初始化（title_label 还未创建）时直接返回，避免早期调用崩溃
    if (title_label == NULL || title == NULL) {
        return;
    }
    // 上锁
    lvgl_port_lock(0);
    lv_label_set_text(title_label, title);
    // 解锁
    lvgl_port_unlock();
}
/* 修改文本的方法 */
void xiaozhi_lvgl_set_text(char *text)
{
    // 上锁
    lvgl_port_lock(0);
    lv_label_set_text(text_label, text);
    // 解锁
    lvgl_port_unlock();
}
/* 修改emoji的方法 */
void xiaozhi_lvgl_set_emoji(char *emoji_name)
{
    bool is_find = false;
    // 上锁
    lvgl_port_lock(0);
    for (int i = 0; i < 21; i++) {
        if (strcmp(emoji_name, emoji_list[i].name) == 0) {
            lv_label_set_text(emoji_label, emoji_list[i].emoji);
            is_find = true;
            break;
        }
    }

    if (!is_find) {
        lv_label_set_text(emoji_label, "🙂");
    }
    // 解锁
    lvgl_port_unlock();
}

/* 修改标题栏右侧的北京时间（"YYYY-MM-DD HH:MM:SS"） */
void xiaozhi_lvgl_set_datetime(const char *datetime)
{
    // 界面未初始化 / 传入空指针时直接返回，避免早期调用崩溃
    if (datetime_label == NULL || datetime == NULL) {
        return;
    }
    // 上锁
    lvgl_port_lock(0);
    lv_label_set_text(datetime_label, datetime);
    // 解锁
    lvgl_port_unlock();
}

/* 更新标题栏最右侧的 WiFi 状态标识（由 xiaozhi_wifi 的 wifi/ip 事件驱动） */
void xiaozhi_lvgl_set_wifi_state(xiaozhi_wifi_state_t state)
{
    if (state != XIAOZHI_WIFI_STATE_CONNECTING && state != XIAOZHI_WIFI_STATE_CONNECTED &&
        state != XIAOZHI_WIFI_STATE_FAILED) {
        return;
    }
    // 先缓存状态：界面未初始化（wifi_label 还没创建）时也要记下来，show_init 会补显示
    s_wifi_state = state;
    if (wifi_label == NULL) {
        return;
    }
    // 上锁
    lvgl_port_lock(0);
    xiaozhi_lvgl_wifi_apply();
    // 解锁
    lvgl_port_unlock();
}