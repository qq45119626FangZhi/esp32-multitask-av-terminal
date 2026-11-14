#include "adc_button.h"
#include "esp_log.h"
#include "xiaozhi_wifi.h"
#include <stdio.h>

#include "xiaozhi_active.h"
#include "xiaozhi_audio.h"
#include "xiaozhi_audio_decoder.h"
#include "xiaozhi_audio_encoder.h"
#include "xiaozhi_common.h"
#include "xiaozhi_lvgl.h"
#include "xiaozhi_sr.h"
#include "xiaozhi_ws.h"
#include "xiaozhi_appmode.h"
#include "xiaozhi_doorbell.h"
#include "xiaozhi_sdcard.h"
#include "xiaozhi_camera.h"
#include "xiaozhi_camview.h"
#include "xiaozhi_led.h"
#include "xiaozhi_time.h"
#include "xiaozhi_recorder.h"
#include "xiaozhi_http.h"
#include "xiaozhi_ota.h"

void adc_button_cb(void *arg, void *usr_data);
// 创建所有的环形缓冲区
void create_all_ring_buffer(void);
// 注册被唤醒的回调
void xiaozhi_wakeup_callback(void);
// vad的状态发送改变的回调
void xiaozhi_sr_vad_change_callback(void);
// 处理文本帧数据的方法
void xiaozhi_process_text_frame(char *text_frame);
// 处理二进制音频数据帧的方法
void process_binary_audio_frame(const char *bin_data, int data_len);
/* 创建一个任务，用于处理小智的音频数据发送至服务器 */
void xiaozhi_ws_send_audio_task(void *pvParameters);
/* 封装一个测试任务，用于短接两个环形缓冲区 */
void xiaozhi_ringbuffer_test(void *pvParameters);
#define TAG "main"

xiaozhi_handle_t xiaozhi_handle;

typedef enum {
    SW2_SINGLE_CLICK,
    SW2_LONG_PRESS_START,
    SW3_SINGLE_CLICK,
    SW3_LONG_PRESS_START,
    /* 新增事件一律追加在末尾：枚举值被强转成指针当 usr_data 传，改顺序会串事件 */
    SW2_DOUBLE_CLICK,
} adc_button_event_t;
void app_main(void)
{
    // 创建所有的环形缓冲区
    create_all_ring_buffer();
    // 创建ws相关的事件标志组
    xiaozhi_handle.ws_event_group = xEventGroupCreate();
    // 注册被唤醒的回调
    xiaozhi_handle.wakeup_callback = xiaozhi_wakeup_callback;
    //  注册vad改变的回调
    xiaozhi_handle.vad_change_callback = xiaozhi_sr_vad_change_callback;
    // 注册处理文本帧数据的方法
    xiaozhi_handle.process_text_frame = xiaozhi_process_text_frame;
    // 注册处理二进制音频数据帧的方法
    xiaozhi_handle.process_binary_audio_frame = process_binary_audio_frame;

    // 模式仲裁模块初始化（语音 / 门铃 / 监控 三模式互斥）
    xiaozhi_appmode_init();
    // Initialize ADC button
    adc_button_init();
    // 注册sw2注册单击事件的回调
    adc_button_register_sw2_callback(BUTTON_SINGLE_CLICK, adc_button_cb, (void *)SW2_SINGLE_CLICK);
#if XIAOZHI_APPMODE_SW2_LONG_DOORBELL
    // 注册sw2长按事件的回调（门铃触发）
    adc_button_register_sw2_callback(BUTTON_LONG_PRESS_START, adc_button_cb, (void *)SW2_LONG_PRESS_START);
#endif
#if XIAOZHI_APPMODE_SW2_DOUBLE_DETAIL
    // 注册sw2双击事件的回调（打开/关闭设备详情页）
    adc_button_register_sw2_callback(BUTTON_DOUBLE_CLICK, adc_button_cb, (void *)SW2_DOUBLE_CLICK);
#endif
    // 注册sw3的单击事件回调
    adc_button_register_sw3_callback(BUTTON_SINGLE_CLICK, adc_button_cb, (void *)SW3_SINGLE_CLICK);
    // 注册sw3的长按事件回调
    adc_button_register_sw3_callback(BUTTON_LONG_PRESS_START, adc_button_cb, (void *)SW3_LONG_PRESS_START);
    // lvgl初始化
    xiaozhi_lvgl_init();
    // 界面就绪后刷新一次标题（显示当前工作模式）
    xiaozhi_appmode_refresh_ui();
    // SD卡存储初始化（M1）：上电挂载 + 容量/写速日志 + 拔卡自动重挂载
    // XIAOZHI_SD_ENABLE=0 时为空实现，不影响既有语音链路
    xiaozhi_sdcard_init();
    // OTA 升级（M7）：打印运行槽/版本/目标槽，创建 /sdcard/OTA 目录。
    // 必须在 xiaozhi_sdcard_init() 之后、xiaozhi_http_init() 之前（HTTP 路由要用到本模块）
    xiaozhi_ota_init();
    // 首页右下角显示固件版本（V1.0），版本号唯一来源是 CMakeLists 的 project(... VERSION ...)
    xiaozhi_lvgl_set_version(xiaozhi_ota_version_short());
    // wifi的初始化~
    xiaozhi_wifi_init();
    // wifi 已连上（xiaozhi_wifi_init 阻塞到拿到 IP）：SNTP 校时一次（CST-8 北京时间），
    // 成功后标题栏右侧显示 "YYYY-MM-DD HH:MM:SS"，未授时保持 "--"
    xiaozhi_time_start();
    // 获取验证码的初始化
    xiaozhi_active_init();
    // ES8311的初始化
    xiaozhi_audio_init();
    // 摄像头初始化（M2）：SCCB 复用上面刚创建的 I2C 总线，必须放在 xiaozhi_audio_init() 之后
    // 默认不再上电初始化：XIAOZHI_CAM_BOOT_INIT=0 时摄像头由监控模式钩子按需启动
    // （置 1 可恢复 M2 的“上电初始化 + 抓拍自检”验证行为）
#if XIAOZHI_CAM_BOOT_INIT
    xiaozhi_camera_init(XIAOZHI_CAM_FMT_JPEG);
    // 上电抓拍自检：抓 XIAOZHI_CAM_BOOT_SNAPSHOTS(3) 张 QVGA JPEG 到 SD
    xiaozhi_camera_boot_snapshots(XIAOZHI_CAM_BOOT_SNAPSHOTS);
#endif
    // 摄像头预览视图：默认只注册监控模式钩子（不建画面、不启用摄像头）；
    // 切到监控模式 = 启动摄像头 + 全屏预览，切走 = 关闭画面 + 释放摄像头
    xiaozhi_camview_init();
    // 状态灯（WS2812 ×2，DIN=GPIO46）：监控模式摄像头画面打开后点亮 LED2 绿色，
    // 切到语音/门铃模式（画面关闭）自动熄灭；XIAOZHI_LED_ENABLE=0 时为空实现
    xiaozhi_led_init();
    // 门铃业务初始化（注册门铃模式钩子：响铃/屏显/事件日志/录像钩子）
    xiaozhi_doorbell_init();
    // M5 录像初始化：监控模式连续录 MJPEG-AVI（3 分钟切片、循环覆盖、卡满保护、
    // 掉盘降级），并向门铃注入事件录像后端。须在 camview / doorbell 之后调用
    // （同一模式的钩子按注册顺序执行：先开预览，再开录像）
    xiaozhi_recorder_init();
    // ! 初始化小智的状态为IDLE
    xiaozhi_handle.xiaozhi_state = XIAOZHI_STATE_IDLE;
    // SR的初始化.0
    xiaozhi_sr_init();
    // 编码器encoder的初始化
    xiaozhi_audio_encoder_init();
    // 解码器decoder的初始化
    xiaozhi_audio_decoder_init();
    // ws初始化
    xiaozhi_ws_init();
    // M6 局域网 Web（esp_http_server）：手机浏览器访问板子 IP 即可
    //   浏览/下载 SD 卡文件、看 MJPEG 实时预览（预览只在监控模式可用）
    // 放在最后启动，避免 httpd 的栈/缓冲占用内部 RAM 影响音频与 SR 初始化
    xiaozhi_http_init();

    /* 栈放 PSRAM（xiaozhi_task_create 统一处理，失败自动回落内部 RAM） */
    xiaozhi_task_create(xiaozhi_ws_send_audio_task, "xiaozhi_ws_send_audio_task", XIAOZHI_STACK_16K, NULL,
                        XIAOZHI_PRIO_AUDIO, NULL, 1);

    // OTA 回滚保护确认：启动流程走到这里说明新固件能正常跑起来，
    // 调用 esp_ota_mark_app_valid_cancel_rollback() 取消"下次自动回退旧分区"。
    // 注意：本调用必须在所有关键初始化之后，否则新固件有缺陷也来不及回退。
    xiaozhi_ota_mark_valid();
}

void adc_button_cb(void *arg, void *usr_data)
{
    uint8_t event_type = (uint8_t)usr_data;
    switch (event_type) {
        case SW2_SINGLE_CLICK:
            ESP_LOGI(TAG, "SW2 SINGLE CLICK");
#if XIAOZHI_APPMODE_SW2_SINGLE_SWITCH
            // 切换工作模式：语音 -> 门铃 -> 监控 -> 语音
            xiaozhi_appmode_button_switch();
#endif
            break;
        case SW2_LONG_PRESS_START:
            ESP_LOGI(TAG, "SW2 LONG PRESS START");
#if XIAOZHI_APPMODE_SW2_LONG_DOORBELL
            // 门铃触发：切到门铃模式（已在该模式时作为重复触发）
            xiaozhi_appmode_button_doorbell();
#endif
            break;
        case SW3_SINGLE_CLICK:
            ESP_LOGI(TAG, "SW3 SINGLE CLICK");
#if XIAOZHI_APPMODE_SW3_SINGLE_SWITCH
            // 切换工作模式：语音 -> 门铃 -> 监控 -> 语音
            xiaozhi_appmode_button_switch();
#else
            // SW3 单击：显示/隐藏屏幕最下一行的推流地址（显示时在其它 UI 之上）
            xiaozhi_lvgl_toggle_stream_url();
#endif
            break;
        case SW3_LONG_PRESS_START:
            ESP_LOGI(TAG, "SW3 LONG PRESS START");
            xiaozhi_wifi_reset();
            break;
        case SW2_DOUBLE_CLICK:
            ESP_LOGI(TAG, "SW2 DOUBLE CLICK");
#if XIAOZHI_APPMODE_SW2_DOUBLE_DETAIL
            // 设备详情页（版本 / Flash / PSRAM / SD），再双击一次关闭
            xiaozhi_lvgl_toggle_device_detail();
#endif
            break;
        default:
    }
}

void create_all_ring_buffer(void)
{
    // sr2encoder的环形缓冲区（本体一律放 PSRAM，规格见 xiaozhi_common.h）
    xiaozhi_handle.ring_buffer_sr2encoder_handle = xiaozhi_ringbuf_create(XIAOZHI_RINGBUF_16K, RINGBUF_TYPE_BYTEBUF);
    // encoder2ws的环形缓冲区
    xiaozhi_handle.ring_buffer_encoder2ws_handle = xiaozhi_ringbuf_create(XIAOZHI_RINGBUF_16K, RINGBUF_TYPE_NOSPLIT);
    // ws2decoder的环形缓冲区
    xiaozhi_handle.ring_buffer_ws2decoder_handle = xiaozhi_ringbuf_create(XIAOZHI_RINGBUF_16K, RINGBUF_TYPE_NOSPLIT);
}

/* 封装一个测试任务，用于短接两个环形缓冲区 */
void xiaozhi_ringbuffer_test(void *pvParameters)
{
    // while (1) {
    //     size_t size = 0;
    //     // 从encoder2ws的环形缓冲区提取数据
    //     void *rev_pointer = xRingbufferReceive(xiaozhi_handle.ring_buffer_encoder2ws_handle, &size, portMAX_DELAY);
    //     // 放入至ws2decoder的环形缓冲区
    //     xRingbufferSend(xiaozhi_handle.ring_buffer_ws2decoder_handle, rev_pointer, size, portMAX_DELAY);
    //     // 释放环形缓冲区
    //     vRingbufferReturnItem(xiaozhi_handle.ring_buffer_encoder2ws_handle, rev_pointer);
    // }
}

// 注册被唤醒的回调
void xiaozhi_wakeup_callback(void)
{
    // 模式仲裁门控：仅当当前模式允许语音时才走原有唤醒流程（默认关闭门控，行为不变）
    if (!xiaozhi_appmode_voice_enabled()) {
        ESP_LOGI(TAG, "voice disabled in mode: %s", xiaozhi_appmode_name(xiaozhi_appmode_get()));
        return;
    }

    if (xiaozhi_handle.xiaozhi_state == XIAOZHI_STATE_IDLE) {
        // 发起建立连接
        bool is_connected = xiaozhi_ws_connect_sync();
        if (is_connected) {
            // 发送hello
            xiaozhi_ws_send_hello();
            // 以阻塞的形式等待hello成功
            EventBits_t res = xEventGroupWaitBits(xiaozhi_handle.ws_event_group, WS_HELLO_SENT_BIT, pdTRUE, pdFALSE, portMAX_DELAY);
            if (!(res & WS_HELLO_SENT_BIT)) {
                ESP_LOGI(TAG, "send hello failed");
            } else {
                //! 发送iot的相关文本
                xiaozhi_ws_send_iot_state();
                xiaozhi_ws_send_iot_desc();
                // 发送唤醒词
                xiaozhi_ws_send_wkup_word();
            }
        }
    } else if (xiaozhi_handle.xiaozhi_state == XIAOZHI_STATE_SPEAKING) {
        // 发送终止消息，打断小智说话
        xiaozhi_ws_send_abort();
        // 发送唤醒词
        xiaozhi_ws_send_wkup_word();
    }
}

// vad的状态发送改变的回调
void xiaozhi_sr_vad_change_callback(void)
{
#if XIAOZHI_DOORBELL_RING_VAD_GATE
    // 门铃响铃期间屏蔽 VAD：喇叭的"叮咚"会被 MIC 拾取，避免自激/误唤醒
    if (xiaozhi_doorbell_is_ringing()) {
        return;
    }
#endif

    // 检测到人声  1. 我们在说话  2. 小智在说话
    if (xiaozhi_handle.vad_current_state == VAD_SPEECH) {
        // if (xiaozhi_handle.xiaozhi_state != XIAOZHI_STATE_SPEAKING)
        if (xiaozhi_handle.xiaozhi_state == XIAOZHI_STATE_IDLE) {
            xiaozhi_handle.xiaozhi_state = XIAOZHI_STATE_LISTENING;
            // 发送开始监听
            xiaozhi_ws_send_start_listening();
        }
    }

    // 检测到静默 1. 我们没有说话  2. 小智在说话，但是卡顿了(网络原因)
    if (xiaozhi_handle.vad_current_state == VAD_SILENCE) {
        if (xiaozhi_handle.xiaozhi_state != XIAOZHI_STATE_SPEAKING) {
            xiaozhi_handle.xiaozhi_state = XIAOZHI_STATE_IDLE;
            // 发送停止监听
            xiaozhi_ws_send_stop_listening();
        }
    }
}
// 处理文本帧数据的方法
void xiaozhi_process_text_frame(char *text_frame)
{
    cJSON *root = cJSON_Parse(text_frame);
    if (root == NULL) {
        ESP_LOGI(TAG, "parse json failed");
        return;
    }

    cJSON *type = cJSON_GetObjectItem(root, "type");
    if (!cJSON_IsString(type)) {
        ESP_LOGW(TAG, "text frame without type: %s", text_frame);
        cJSON_Delete(root);
        return;
    }
    if (strcmp(type->valuestring, "hello") == 0) {
        // ! 修改hello回应成功的标志位
        xEventGroupSetBits(xiaozhi_handle.ws_event_group, WS_HELLO_SENT_BIT);
    }

    // 判断是否是emoji
    if (strcmp(type->valuestring, "llm") == 0) {
        char *emotion = cJSON_GetObjectItem(root, "emotion")->valuestring;
        xiaozhi_lvgl_set_emoji(emotion);
    }

    // 判断是否是文本内容
    if (strcmp(type->valuestring, "tts") == 0) {
        char *state = cJSON_GetObjectItem(root, "state")->valuestring;
        if (strcmp(state, "sentence_start") == 0) {
            char *text = cJSON_GetObjectItem(root, "text")->valuestring;
            xiaozhi_lvgl_set_text(text);
        }

        // 判断小智开始说话
        if (strcmp(state, "start") == 0) {
            // 小智开始说话
            xiaozhi_handle.xiaozhi_state = XIAOZHI_STATE_SPEAKING;
        }
        // 判断小智停止说话
        if (strcmp(state, "stop") == 0) {
            // 小智开始说话
            xiaozhi_handle.xiaozhi_state = XIAOZHI_STATE_IDLE;
        }
    }

    //语音切换的功能
    if (strcmp(type->valuestring, "iot") == 0) {
        cJSON *cmdsItem = cJSON_GetObjectItem(root, "commands");
        cJSON *cmdItem = cJSON_GetArrayItem(cmdsItem, 0);
        if (cmdItem == NULL) {
            // 携带 states/descriptors 的 iot 帧（状态同步）不处理，避免空指针
            ESP_LOGI(TAG, "iot frame without commands, ignored");
        } else {
            cJSON *parametersItem = cJSON_GetObjectItem(cmdItem, "parameters");
            cJSON *methodItem = cJSON_GetObjectItem(cmdItem, "method");
            const char *method = cJSON_IsString(methodItem) ? methodItem->valuestring : NULL;

            if (method == NULL) {
                ESP_LOGW(TAG, "iot command without method: %s", text_frame);
            } else if (strcmp(method, "SetVolume") == 0) {
                cJSON *volumeItem = cJSON_GetObjectItem(parametersItem, "volume");
                if (cJSON_IsNumber(volumeItem)) {
                    xiaozhi_audio_volume_set(volumeItem->valueint);
                }
            } else if (strcmp(method, "SetMute") == 0) {
                cJSON *muteItem = cJSON_GetObjectItem(parametersItem, "mute");
                bool is_mute = false;
                if (cJSON_IsBool(muteItem)) {
                    is_mute = cJSON_IsTrue(muteItem);
                }
                xiaozhi_audio_mute_set(is_mute);
            } else if (strcmp(method, "SetMode") == 0 || strcmp(method, "Mode.SetMode") == 0) {
                // 语音口令切换工作模式（语音 / 门铃 / 监控）
                ESP_LOGI(TAG, "iot SetMode received");
                xiaozhi_appmode_voice_command(parametersItem);
            } else {
                ESP_LOGW(TAG, "unknown iot method: %s", method);
            }
        }
    }

    // 语音口令兜底：云端只回文字、不下发 Mode.SetMode 时，用 ASR 文本本地切模式
    if (strcmp(type->valuestring, "stt") == 0) {
        cJSON *textItem = cJSON_GetObjectItem(root, "text");
        if (cJSON_IsString(textItem)) {
            xiaozhi_appmode_voice_text(textItem->valuestring);
        }
    }

    cJSON_Delete(root);
}

// 处理二进制音频数据帧的方法
void process_binary_audio_frame(const char *bin_data, int data_len)
{
    // 将二进制音频数据存入至ws2decoder的环形缓冲区
    xRingbufferSend(xiaozhi_handle.ring_buffer_ws2decoder_handle, bin_data, data_len, portMAX_DELAY);
}

/* 创建一个任务，用于处理小智的音频数据发送至服务器 */
void xiaozhi_ws_send_audio_task(void *pvParameters)
{
    while (1) {
        // 从encoder2ws的环形缓冲区中取出数据
        size_t rev_size = 0;
        void *rev_pointer = xRingbufferReceive(xiaozhi_handle.ring_buffer_encoder2ws_handle, &rev_size, portMAX_DELAY);
        // ! 一定要保证小智正在listening的时候才发送音频数据
        // ESP_LOGI(TAG, "rev_size: %d", xiaozhi_handle.xiaozhi_state);
        if (xiaozhi_handle.xiaozhi_state == XIAOZHI_STATE_LISTENING) {
            // 发送音频opus数据至服务器
            xiaozhi_ws_send_bin(rev_pointer, rev_size);
        }

        // 释放环形缓冲区
        vRingbufferReturnItem(xiaozhi_handle.ring_buffer_encoder2ws_handle, rev_pointer);
    }
}