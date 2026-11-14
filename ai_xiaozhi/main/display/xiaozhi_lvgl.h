#pragma once
#include <stdbool.h>

#include "esp_log.h"
#include "xiaozhi_lcd.h"
#include "esp_lvgl_port.h"
#include "font_emoji.h"

void xiaozhi_lvgl_init(void);

/* 公用屏幕对象（lv_screen_active() 的唯一来源，首次调用时缓存）。
 * 所有模块创建控件都应挂到它上面，不要再各自调用 lv_screen_active()——
 * 现在只有一个默认屏，两者等价；一旦将来出现第二屏（lv_screen_load），
 * 各自取活动屏会把控件挂到"当时的"屏幕上，表现为画面/文本莫名消失。
 * 界面未初始化时返回 NULL，调用方必须判空。 */
lv_obj_t *xiaozhi_lvgl_screen(void);

/* ==========================================================================
 * 视频视图（监控全屏预览 / 叠加预览）
 *   目的：把"LVGL 图像对象"的全部细节（创建、样式、置顶、隐藏、销毁、
 *   字节序、端口锁、脏区重绘）收在本模块里；业务侧（camview 等）只提供
 *   一块 RGB565 像素缓冲，不再出现任何 lv_* / lvgl_port_* 调用。
 *
 *   缓冲生命周期由业务侧负责：open 时传入的指针必须一直有效到
 *   xiaozhi_lvgl_video_close() 成功返回为止（本模块不拷贝、不释放它）。
 * ========================================================================== */

/* 视图配置：像素缓冲 + 尺寸 + 形态 */
typedef struct {
    const void *rgb565; /* RGB565 像素缓冲首地址（外部持有，本模块只借用） */
    int w;              /* 宽（像素） */
    int h;              /* 高（像素） */
    bool fullscreen;    /* true = 铺满全屏、无边框、置顶；false = 右上角叠加、带 1px 边框 */
    int offset_x;       /* 叠加模式的右上角偏移（全屏忽略） */
    int offset_y;
} xiaozhi_lvgl_video_cfg_t;

/* 打开视图（重复调用等价于 rebind：更新缓冲指针/尺寸/形态）。
 * 返回值：ESP_OK / ESP_ERR_INVALID_ARG / ESP_ERR_INVALID_STATE（界面未就绪）/ ESP_ERR_TIMEOUT（取锁超时） */
esp_err_t xiaozhi_lvgl_video_open(const xiaozhi_lvgl_video_cfg_t *cfg);

/* 仅更新缓冲地址或尺寸（对象已存在时复用，不重建控件） */
esp_err_t xiaozhi_lvgl_video_rebind(const void *rgb565, int w, int h);

/* 关闭并销毁视图对象；像素缓冲仍归业务侧，本模块不动它 */
esp_err_t xiaozhi_lvgl_video_close(void);

/* 视图对象是否存在 */
bool xiaozhi_lvgl_video_is_open(void);

/* 写入一帧：在 LVGL 端口锁内执行 paint(ctx) 写像素，随后标脏重绘。
 * paint 执行期间已持锁，**不得**在 paint 内再调 lvgl_port_lock、不得阻塞。
 * timeout_ms 内拿不到锁则本帧直接丢弃（返回 ESP_ERR_TIMEOUT），不阻塞采集。 */
esp_err_t xiaozhi_lvgl_video_update(void (*paint)(void *ctx), void *ctx, uint32_t timeout_ms);

/* 显示/隐藏（隐藏后不再刷新，但对象保留） */
void xiaozhi_lvgl_video_set_visible(bool visible);
bool xiaozhi_lvgl_video_is_visible(void);

/* 显示hello world */
void xiaozhi_lvgl_show_hello_world(void);

/* 显示二维码 */
void xiaozhi_lvgl_show_qrcode(char *qrcode_data);

/* 删除二维码 */
void xiaozhi_lvgl_delete_qrcode(void);

/* 修改标题的方法 */
void xiaozhi_lvgl_set_title(char *title);
/* 修改文本的方法 */
void xiaozhi_lvgl_set_text(char *text);
/* 修改emoji的方法 */
void xiaozhi_lvgl_set_emoji(char *emoji_name);

/* 底部推流地址条（形如 "上位机连接：http://10.41.181.139/"）：
 * SW3 单击调用本函数在“显示 / 隐藏”之间切换；显示时固定占屏幕最下一行
 * （宽 320、高 20），并置顶到标题/文本/监控全屏画面之上（层级最高）。
 * 地址取自 xiaozhi_http_url()，每次显示时重新读取设备实际 IP（WiFi 重连换 IP 也能跟上）；
 * 未拿到 IP 时显示提示文案。返回切换后的可见状态（界面未就绪时返回 false）。 */
bool xiaozhi_lvgl_toggle_stream_url(void);

/* 记录固件版本（如 "V1.0"），由 main.c 在 OTA 模块初始化后写入。
 * 首页不再常驻显示版本号，改在“设备详情页”（SW2 双击打开）里查看。 */
void xiaozhi_lvgl_set_version(const char *ver);

/* 设备详情页（模态，4 行：版本 / Flash / PSRAM / SD）。
 * SW2 双击调用本函数在“打开 / 关闭”之间切换；打开时永远是最上层
 * （盖住普通界面、监控全屏画面、升级面板与底部地址条），数值每次打开时重新采集。
 * 返回切换后的打开状态（界面未就绪时返回 false）。 */
bool xiaozhi_lvgl_toggle_device_detail(void);
bool xiaozhi_lvgl_device_detail_is_open(void);

/* OTA 升级进度面板（居中，覆盖在标题/文本/监控画面之上，但在地址条之下）：
 * show(pct, "接收中"/"写入中"/"校验中"/"升级完成，正在重启…") 显示并刷新，
 * hide() 收起。由 main/ota/xiaozhi_ota.c 在刷写过程中调用。 */
void xiaozhi_lvgl_ota_progress_show(int pct, const char *text);
void xiaozhi_lvgl_ota_progress_hide(void);

/* 修改标题栏右侧的北京时间（"YYYY-MM-DD HH:MM:SS"），由 xiaozhi_time 每秒调用 */
void xiaozhi_lvgl_set_datetime(const char *datetime);

/* 标题栏最右侧的 WiFi 状态标识（语音/门铃模式可见，监控模式全屏预览时不显示） */
typedef enum {
    XIAOZHI_WIFI_STATE_CONNECTING = 0, /* 连接中（上电默认，灰色） */
    XIAOZHI_WIFI_STATE_CONNECTED,      /* 连接成功（拿到 IP，绿色） */
    XIAOZHI_WIFI_STATE_FAILED,         /* 连接失败/断开（红色） */
} xiaozhi_wifi_state_t;

/* 更新 WiFi 状态标识；界面未就绪时只缓存状态，界面创建后自动补显示 */
void xiaozhi_lvgl_set_wifi_state(xiaozhi_wifi_state_t state);