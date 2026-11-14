#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* ==========================================================================
 * OTA 固件升级（双槽 ota_0 / ota_1 + otadata）
 *
 * 两个入口（共用同一套"写 flash"逻辑，只是数据源不同）：
 *   1. 手机推送：HTTP POST /api/ota/upload → 边收边写 flash，
 *      同时把包存一份到 /sdcard/OTA/（Q2 需求：列表里能再点它重装/回退）
 *   2. SD 卡安装：HTTP POST /api/ota/apply?path=OTA/xxx.bin
 *      → 从 SD 读 4 KB 分块写 flash（设备端"已有升级包"的列表就靠这个）
 *
 * 关键约束（详见 main/doc/OTA.md）：
 *   - 固件只能运行在 flash 的 app 分区；SD/PSRAM 只是包的暂存与中转；
 *   - 只有 esp_ota_end() 校验通过（镜像 magic / chip id / SHA256）才切换启动分区，
 *     任何中断/掉电都保持原固件可启动；
 *   - 开启 CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE 后，新固件必须在启动流程末尾调用
 *     xiaozhi_ota_mark_valid()，否则下次重启会被 bootloader 回退到旧分区；
 *   - 本模块只更新 app 分区：srmodels（唤醒词）、bootloader、分区表仍须 USB 重烧。
 * ========================================================================== */

/* 总开关（0 = 所有接口退化为"未启用"，不影响其它功能） */
#define XIAOZHI_OTA_ENABLE 1

/* 升级包目录（SD 卡 VFS 路径与相对路径） */
#define XIAOZHI_OTA_DIR      "/sdcard/OTA"
#define XIAOZHI_OTA_DIR_REL  "OTA"

/* 镜像校验失败/写失败后的重启延迟（留给 App 收到 JSON 响应） */
#define XIAOZHI_OTA_REBOOT_DELAY_MS 1500

/* 一次最多列出多少个包（受 HTTP JSON 缓冲约束） */
#define XIAOZHI_OTA_MAX_PKGS 8

/* 升级状态机 */
typedef enum {
    XIAOZHI_OTA_STATE_IDLE = 0,  /* 空闲 */
    XIAOZHI_OTA_STATE_RECEIVING, /* 正在从手机接收（边收边写） */
    XIAOZHI_OTA_STATE_WRITING,   /* 正在从 SD 卡读包写 flash */
    XIAOZHI_OTA_STATE_VERIFYING, /* 写完后校验镜像 */
    XIAOZHI_OTA_STATE_DONE,      /* 成功，启动分区已切换，等待重启 */
    XIAOZHI_OTA_STATE_ERROR,     /* 失败（msg 里有原因，旧固件仍可启动） */
} xiaozhi_ota_state_t;

/* 升级状态快照（供 /api/ota/status 与屏幕进度条使用） */
typedef struct {
    xiaozhi_ota_state_t state;
    int pct;              /* 0~100 */
    uint32_t bytes_done;
    uint32_t bytes_total;
    char msg[96];         /* 中文状态/错误描述 */
    char src[64];         /* 源包名（手机推送的文件名或 SD 上的文件名） */
} xiaozhi_ota_status_t;

/* SD 卡上的一个升级包 */
typedef struct {
    char name[48];        /* 文件名（不含路径） */
    uint32_t size;        /* 字节数 */
    char version[24];     /* 镜像头里的 esp_app_desc.version，读不到时为 "--" */
    bool valid;           /* 头部 magic 校验是否通过（明显不是 app 镜像时为 false） */
} xiaozhi_ota_pkg_t;

/* 启动时调用一次（在 xiaozhi_sdcard_init() 之后、xiaozhi_http_init() 之前）：
 * 打印运行槽/版本/目标槽信息，创建 /sdcard/OTA 目录，初始化互斥量。 */
void xiaozhi_ota_init(void);

/* 新固件启动流程末尾调用：取消回滚（未开启回滚保护时为空操作）。 */
void xiaozhi_ota_mark_valid(void);

/* 版本与槽位（首页右下角、/api/status 用） */
const char *xiaozhi_ota_version(void);       /* "1.0.0"：取自 esp_app_desc */
const char *xiaozhi_ota_version_short(void); /* "V1.0"：取版本前两段 */
const char *xiaozhi_ota_slot(void);          /* "ota_0" / "ota_1" */

/* 是否正在升级（升级期间拒绝 /stream 与第二个升级请求） */
bool xiaozhi_ota_busy(void);

/* 取状态快照（线程安全） */
void xiaozhi_ota_get_status(xiaozhi_ota_status_t *out);

/* 状态名（JSON 用）：idle/receiving/writing/verifying/done/error */
const char *xiaozhi_ota_state_name(xiaozhi_ota_state_t state);

/* ---------------- 入口 1：手机推送（httpd handler 内同步调用） ---------------- */

/* 内部 RAM 的 4 KB 分块缓冲：httpd handler 用它做 httpd_req_recv()，
 * 再把数据交给 xiaozhi_ota_upload_write()。
 * 注意不能用 PSRAM 缓冲——esp_ota_write 内部会短暂关闭 cache，PSRAM 不可访问；
 * 用静态 .bss 也避免运行时 malloc(4KB) 在内部 RAM 碎片化时失败。 */
uint8_t *xiaozhi_ota_chunk_buf(void);
size_t xiaozhi_ota_chunk_size(void);

/* 开始接收。name = 文件名（会做 basename/后缀清洗），total = Content-Length，
 * save_to_sd = 是否同时存一份到 /sdcard/OTA/。total=0 表示未知（不判大小）。 */
esp_err_t xiaozhi_ota_upload_begin(const char *name, uint32_t total, bool save_to_sd);

/* 写入一段数据（内部按需 esp_ota_write + fwrite 落卡，并刷新进度） */
esp_err_t xiaozhi_ota_upload_write(const void *data, size_t len);

/* 结束接收：校验 → 切换启动分区 → 计划重启。失败时返回错误且不改启动分区 */
esp_err_t xiaozhi_ota_upload_end(void);

/* 中途放弃（断连/超时/写卡失败） */
void xiaozhi_ota_upload_abort(void);

/* ---------------- 入口 2：SD 卡安装（建议在独立任务里调用） ---------------- */

/* rel_path 形如 "OTA/xxx.bin"（相对 /sdcard）或 "/sdcard/OTA/xxx.bin"。
 * 本函数阻塞到安装结束（3.9 MB 约 15~30 s），成功后自动计划重启。 */
esp_err_t xiaozhi_ota_apply_path(const char *rel_path);

/* 异步安装：内部建一个 5 KB / prio 4 的任务执行 xiaozhi_ota_apply_path，
 * 立即返回。httpd handler 用它，避免长时间占用 httpd 任务导致进度查询被卡住。 */
esp_err_t xiaozhi_ota_apply_async(const char *rel_path);

/* ---------------- 列表 / 重启 ---------------- */

/* 扫描 /sdcard/OTA 目录下的 .bin（按文件名排序），返回实际条数（0~max） */
int xiaozhi_ota_list(xiaozhi_ota_pkg_t *out, int max);

/* delay_ms 后重启（用 esp_timer 一次性回调，先让 HTTP 响应发出去） */
void xiaozhi_ota_schedule_reboot(uint32_t delay_ms);
