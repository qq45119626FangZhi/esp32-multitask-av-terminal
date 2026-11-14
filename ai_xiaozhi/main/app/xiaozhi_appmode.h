#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "cJSON.h"

/* ==========================================================================
 * 设备工作模式仲裁（语音 / 门铃 / 监控）
 *   1. 三个模式互斥：同一时刻只有一个模式处于 active 状态；
 *   2. 切换入口：
 *        - 按键：SW2 单击           -> 循环切换下一个模式
 *                SW3 单击           -> 显示/隐藏底部推流地址（宏置 1 可改回切模式）
 *                SW2 长按           -> 门铃触发（切到门铃模式）
 *        - 语音：小智 IoT 指令 Mode.SetMode（见 vol_iot_desc/iot_desc.json）
 *   3. 本模块只做“仲裁 + 生命周期串联”，具体业务（摄像头/SD/录像）由各模块
 *      通过 xiaozhi_appmode_register() 注册 enter/exit/trigger 钩子后自行实现，
 *      互不影响，未实现的功能不会破坏语音链路的既有行为。
 * ========================================================================== */

/* 设备工作模式 */
typedef enum {
    XIAOZHI_MODE_VOICE = 0, /* 语音模式（上电默认）：小智语音聊天 */
    XIAOZHI_MODE_DOORBELL,  /* 门铃模式：预览 + 响铃 + 事件录像（待实现） */
    XIAOZHI_MODE_MONITOR,   /* 监控模式：低帧率连续录像 + 远程查看（待实现） */
    XIAOZHI_MODE_MAX,
} xiaozhi_mode_t;

/* 模式切换来源，用于日志与各模式内的差异化处理 */
typedef enum {
    XIAOZHI_MODE_SRC_BOOT = 0, /* 上电初始化 */
    XIAOZHI_MODE_SRC_BUTTON,   /* 板载按键 */
    XIAOZHI_MODE_SRC_VOICE,    /* 语音口令（云端 iot 指令） */
    XIAOZHI_MODE_SRC_REMOTE,   /* 预留：网络/上位机指令 */
    XIAOZHI_MODE_SRC_TIMEOUT,  /* 预留：业务结束或超时自动返回 */
} xiaozhi_mode_src_t;

/* 各模式可注册的生命周期钩子（均在 appmode 任务上下文中串行执行，不要做长时间阻塞） */
typedef struct {
    /* 进入该模式时调用：申请摄像头/SD/录像等资源 */
    void (*on_enter)(xiaozhi_mode_t mode, xiaozhi_mode_src_t src, void *ctx);
    /* 退出该模式时调用：释放资源，归还内存/带宽 */
    void (*on_exit)(xiaozhi_mode_t mode, void *ctx);
    /* 已处于该模式时再次收到切换请求（例如门铃再次被按下） */
    void (*on_trigger)(xiaozhi_mode_t mode, xiaozhi_mode_src_t src, void *ctx);
    void *ctx; /* 调用者上下文 */
} xiaozhi_mode_ops_t;

/* ---------------- 编译期开关（默认全部保持“现有行为不变”） ---------------- */

/* 语音链路门控：1 = 在监控模式下屏蔽语音唤醒；0 = 不门控（默认，行为与现状一致） */
#define XIAOZHI_APPMODE_VOICE_GATE          0
/* SW2 单击切换模式 */
#define XIAOZHI_APPMODE_SW2_SINGLE_SWITCH   1
/* SW3 单击：0 = 显示/隐藏屏幕底部的推流地址（默认）；
 *           1 = 恢复为“切换模式”（语音 -> 门铃 -> 监控 -> 语音） */
#define XIAOZHI_APPMODE_SW3_SINGLE_SWITCH   0
/* SW2 长按触发门铃（同时切到门铃模式） */
#define XIAOZHI_APPMODE_SW2_LONG_DOORBELL   1
/* SW2 双击：打开/关闭“设备详情页”（版本 / Flash / PSRAM / SD） */
#define XIAOZHI_APPMODE_SW2_DOUBLE_DETAIL   1
/* ASR 文本兜底：云端没有下发 Mode.SetMode 时，直接用识别文本关键词切模式（默认开启） */
#define XIAOZHI_APPMODE_STT_FALLBACK        1

/* ---------------- 对外 API ---------------- */

/* 初始化（main.c 初始化区调用一次，创建请求队列与仲裁任务） */
void xiaozhi_appmode_init(void);

/* 注册某个模式的生命周期钩子（须在 init 之后调用，可在任意时刻注册）。
 *
 * 同一模式支持**多个模块各自注册一格**（M5 起）：进入模式按注册顺序执行，
 * 退出模式按相反顺序执行，因此"后注册的先释放"。
 * 典型用法：监控模式 = xiaozhi_camview（先，负责开摄像头/画面）
 *                      + xiaozhi_recorder（后，负责录像，退出时先收尾）。
 * 每个模式最多 XIAOZHI_APPMODE_MAX_OPS(4) 个，满了返回 ESP_ERR_NO_MEM。 */
esp_err_t xiaozhi_appmode_register(xiaozhi_mode_t mode, const xiaozhi_mode_ops_t *ops);

/* 读取当前模式 / 模式名字（"voice"/"doorbell"/"monitor"） / 是否处于某模式 */
xiaozhi_mode_t xiaozhi_appmode_get(void);
const char *xiaozhi_appmode_name(xiaozhi_mode_t mode);
bool xiaozhi_appmode_is(xiaozhi_mode_t mode);

/* 异步请求切换模式，由 appmode 任务串行执行（可在按键/ws 回调里调用） */
esp_err_t xiaozhi_appmode_request(xiaozhi_mode_t mode, xiaozhi_mode_src_t src);
/* 异步请求切换到下一个模式：语音 -> 门铃 -> 监控 -> 语音 */
esp_err_t xiaozhi_appmode_request_next(xiaozhi_mode_src_t src);

/* 按键入口：单击切模式 / 长按触发门铃 */
void xiaozhi_appmode_button_switch(void);
void xiaozhi_appmode_button_doorbell(void);

/* 语音入口：解析 Mode.SetMode 的 parameters（支持字符串或数字，见 .c 内说明） */
esp_err_t xiaozhi_appmode_voice_command(cJSON *parameters);

/* 语音入口 2（兜底）：对 ASR 识别文本做关键词匹配，命中则请求切换，返回 true
 * 用于云端只回文字、不下发 Mode.SetMode 的场景（XIAOZHI_APPMODE_STT_FALLBACK=1 时生效） */
bool xiaozhi_appmode_voice_text(const char *text);

/* 界面就绪后刷新一次标题（LVGL 初始化完成后调用） */
void xiaozhi_appmode_refresh_ui(void);

/* 语音链路是否允许工作（XIAOZHI_APPMODE_VOICE_GATE=0 时恒为 true） */
bool xiaozhi_appmode_voice_enabled(void);
