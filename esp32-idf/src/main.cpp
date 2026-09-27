// gaga ai 设备固件 —— ESP-IDF 线主入口（ADR-020）
// 本文件只做三件事：模块装配（AppContext 填指针）、按键/信令的事件接线、
// 任务拉起。业务实现全在各自模块里（ADR-038 重构）：
//   state/  状态机/消息/设置     ui/    界面/显示
//   audio/  音频前端/上行泵      ble/   GATT 服务端（发送自带锁）
//   rec/    录音语义（点对点开合）  talk/  realtime 会话
//   motion/ IMU 驱动/唤醒检测     power/ 低功耗
//   debug/  串口调试命令         protocol/ 帧编解码
// 链路（docs/protocol.md）：按键 → Recorder/TalkSession → BLE 帧 → App（哑管道）
// → 服务器；信令下行 → handleJsonSignal 路由 → UI/消息卡。
#include <cstdio>
#include <cstring>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "cJSON.h"

#include "bsp/esp-bsp.h"  // bsp_i2c_get_handle（IMU 挂共享总线）

#include "AppContext.h"
#include "version.h"
#include "compat.h"
#include "input/ButtonHandler.h"
#include "input/PwrKey.h"
#include "input/Rtc.h"
#include "protocol/frame.h"
#include "ble/GattServer.h"
// #include "net/WifiTransport.h"  // WiFi 直连暂缓（内存不足，恢复=取消注释+CMakeLists 加回）
#include "state/AppState.h"
#include "state/MsgLog.h"
#include "state/Settings.h"
#include "ui/Display.h"
#include "ui/Ui.h"
#include "audio/AudioPipe.h"
#include "audio/OpusCodec.h"
#include "audio/UplinkPump.h"
#include "rec/Recorder.h"
#include "talk/TalkSession.h"
#include "motion/Qmi8658.h"
#include "motion/Wake.h"
#include "power/Power.h"
#include "voice/KeywordWake.h"
#include "debug/SerialCmd.h"

using namespace gaga;

static const char* TAG = "gaga";

// ---- 模块实例（装配顺序即生命周期，全部静态）----
static AppState    appState;
static MsgLog      msgLog;
static Settings    settings;
static Ui          ui;
static AudioPipe   audio;
static GattServer  gatt;      // 外出模式的链路
static TalkSession talk;
static OpusEnc     opusEnc;
static Recorder    recorder;
static UplinkPump  pump;
static Qmi8658     imu;
static Wake        wake;
// static WifiTransport wifiLink;  // 家模式链路（暂缓启用）
static Power       power;
static KeywordWake kws;
static SerialCmd   serial;

static AppContext ctx;  // 装配上下文：全模块经它互相看见

// BOOT 键 = 物理右下键（GPIO0，低有效——实测见 docs/hardware.md）
static constexpr gpio_num_t PIN_BTN_BOOT = GPIO_NUM_0;

// ---------------------------------------------------------------- BLE 帧路由 ----
// 事件泵：RX 帧在 NimBLE host 任务里只拷贝入队，业务在 app 任务消费
//（host 任务里不做 JSON 解析/UI/播放，避免阻塞 BLE 协议栈）
struct BleFrameEvent {
    uint8_t  type;
    uint16_t len;
    uint8_t* data;  // malloc，消费方 free
};
QueueHandle_t bleQ_ = nullptr;  // 非 static：TalkSession 的下行速率仪表 extern 引用

// NimBLE host 任务回调：只拷贝入队就返回（帧泵的"进水管"半边）
static void onBleFrame(uint8_t type, const uint8_t* payload, uint16_t len) {
    if (bleQ_ == nullptr) return;
    BleFrameEvent ev = {};
    ev.type = type;
    ev.len  = len;
    // PSRAM：reply 全文帧可达 ~2KB，内部堆低点只剩几 KB，撞上即丢消息
    ev.data = static_cast<uint8_t*>(
        heap_caps_malloc(static_cast<size_t>(len) + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (ev.data == nullptr) {
        ESP_LOGW(TAG, "[ble] 帧入队分配失败，丢弃");
        return;
    }
    if (len > 0) memcpy(ev.data, payload, len);
    if (xQueueSend(bleQ_, &ev, 0) != pdTRUE) {
        free(ev.data);
        ESP_LOGW(TAG, "[ble] 帧队列满，丢弃 type=0x%02X", type);
    }
}

// JSON 信令路由（protocol.md §3 / data-model.md §1）——下行信令各干各的：
//   talk_*    → TalkSession 自解析（talk_ready 建会话 / talk_asr、talk_reply 上屏 / talk_end 收尾）
//   receipt   → 语音已送达飞书：ASR 终稿填"你："栏，sending → sent
//   reply     → 答案文本填"GAGA："栏 + "叮咚"，息屏时亮屏直达详情页
//   error     → 服务端报错：卡片标"发送失败：msg"
//   hello_ack → 没有专属分支：纯对时信令，envelope 的 ts 在下面统一吃掉
static void handleJsonSignal(const char* json) {
    ESP_LOGI(TAG, "[ble] json: %s", json);
    cJSON* root = cJSON_Parse(json);
    if (root == nullptr) return;
    // envelope 可选 ts（Unix 秒）：服务端信令自动携带，收到即对时（RTC 断电走时自校准）
    const cJSON* ts = cJSON_GetObjectItem(root, "ts");
    if (cJSON_IsNumber(ts) && ts->valuedouble > 1600000000.0) {
        rtcSetUnix(static_cast<int64_t>(ts->valuedouble));
    }
    // 设备过滤（2026-09-27 多设备串扰修复）：receipt/reply/error 服务端盖
    // device 戳（消息归属设备）；带戳且不是本机的直接忽略——手表和胸前
    // 嘎嘎同群同 broker，不过滤的话手表的 ASR 文本会经 FIFO 灌进本机卡片
    //（真机实锤）。不带戳 = 旧服务端/广播信令，维持兼容不过滤。
    const cJSON* dv = cJSON_GetObjectItem(root, "device");
    const cJSON* type = cJSON_GetObjectItem(root, "type");
    const char* t = cJSON_IsString(type) ? type->valuestring : "";
    const bool needsDevice = strncmp(t, "receipt", 7) == 0 ||
                              strncmp(t, "reply", 5) == 0 ||
                              strncmp(t, "error", 5) == 0;
    if (needsDevice && cJSON_IsString(dv) && dv->valuestring[0] != '\0' &&
        strcmp(dv->valuestring, DEVICE_ID) != 0) {
        ESP_LOGI(TAG, "[ble] 信令属设备 %s（非本机 %s），忽略",
                 dv->valuestring, DEVICE_ID);
        cJSON_Delete(root);
        return;
    }
    if (strncmp(t, "talk_", 5) == 0) {
        talk.onSignalJson(json);  // talk_* 全转交（内部自解析状态和字幕）
    } else if (strcmp(t, "receipt") == 0) {
        const cJSON* text = cJSON_GetObjectItem(root, "text");
        const cJSON* mid = cJSON_GetObjectItem(root, "msg_id");
        msgLog.fillAsk(cJSON_IsString(text) ? text->valuestring : "",
                       cJSON_IsString(mid) ? mid->valuestring : "");
        // 录音中全场静默（用户拍板）：不刷 UI；状态切换照常
        //（notifyReceipt 自带 Sending 状态守卫，录音态下自然 no-op）
        if (!appState.isRecording()) {
            ui.onLogChanged();
        }
        appState.notifyReceipt();   // sending → sent ✅
        // 反馈设计（2026-09-25 用户拍板删"✓ 已送达"提示条）：卡片"识别中…"
        // 原地变成识别文字即送达反馈，足够了；提示条常在录下一条时弹出捣乱。
        // showNote 只保留真正要紧的警告（蓝牙断开/错误消息）。
    } else if (strcmp(t, "reply") == 0) {
        const cJSON* text = cJSON_GetObjectItem(root, "text");
        const cJSON* rto = cJSON_GetObjectItem(root, "reply_to");
        if (cJSON_IsString(text)) {
            const int idx = msgLog.fillReply(
                text->valuestring,
                cJSON_IsString(rto) ? rto->valuestring : "");
            // 录音中全场静默（用户拍板）：声音/卡片刷新/自动跳详情全部不做，
            // 只把回复数据记好——松手后卡片状态自然就位
            if (!appState.isRecording()) {
                audio.event(AudioPipe::SndEv::Received);  // 三声 = 有新消息
                if (idx >= 0) {
                    const uint32_t id = msgLog.at(idx)->id;
                    if (appState.screen() == ScreenState::Off) {
                        ui.openDetail(id);
                    } else {
                        ui.onLogChanged();
                    }
                }
            }
        }
    } else if (strcmp(t, "link") == 0) {
        // 手机侧链路状态推送（ADR-038，App 本地信令不经服务器）：
        // mqtt=true/false。设备据此在按键预检时拦下"对着空气说话"。
        const cJSON* mqtt = cJSON_GetObjectItem(root, "mqtt");
        if (cJSON_IsBool(mqtt)) {
            const bool up = cJSON_IsTrue(mqtt);
            if (up != appState.mqttLink()) {   // 仅状态变化时提示，重连不吵
                appState.setMqttLink(up);
                ESP_LOGI(TAG, "[link] 手机侧 MQTT %s", up ? "已连接" : "断开");
                if (!up) ui.showNote("手机没连上服务器");
            }
        }
    } else if (strcmp(t, "error") == 0) {
        const cJSON* msg = cJSON_GetObjectItem(root, "msg");
        const char* m = cJSON_IsString(msg) ? msg->valuestring : "错误";
        msgLog.failSending(m);
        ui.onLogChanged();
        ui.showNote(m);
        appState.notifyError();     // sending → idle
    } else if (strcmp(t, "wifi_cfg") == 0) {
        // 家模式 WiFi 凭证下发（ADR-039）：App 经 BLE 推来，存 NVS。
        // B 期（WiFi 直连 MQTT）读取；本期能力边界=仅存储+回显
        const cJSON* ssid = cJSON_GetObjectItem(root, "ssid");
        const cJSON* pass = cJSON_GetObjectItem(root, "pass");
        if (cJSON_IsString(ssid) && ssid->valuestring[0] != '\0') {
            settings.setWifi(cJSON_IsString(ssid) ? ssid->valuestring : "",
                             cJSON_IsString(pass) ? pass->valuestring : "");
            settings.save();
            ui.showNote("WiFi 已配置 ✓");
            ESP_LOGI(TAG, "[wifi] 凭证已存（%s），B 期启用直连", settings.wifiSsid());
        } else {
            ui.showNote("WiFi 配置无效");
        }
    } else if (strcmp(t, "set_name") == 0) {
        // 设备显示名（ADR-064）：App 经 BLE 下发，存 NVS，hello/rec_start 随身带。
        // 权威存储在设备端——App 只当设置入口，哑管道纪律不破（同 wifi_cfg 模式）
        const cJSON* nm = cJSON_GetObjectItem(root, "name");
        if (cJSON_IsString(nm) && nm->valuestring[0] != '\0') {
            settings.setDevName(nm->valuestring);
            settings.save();
            char note[48];
            snprintf(note, sizeof(note), "已改名「%.24s」✓", settings.devName());
            ui.showNote(note);
            ESP_LOGI(TAG, "[name] 设备显示名已存：%s", settings.devName());
        } else if (cJSON_IsString(nm)) {
            settings.setDevName("");   // 空串 = 清除，回退设备 ID 署名
            settings.save();
            ui.showNote("已恢复默认名 ✓");
        } else {
            ui.showNote("名字无效");
        }
    }
    cJSON_Delete(root);
}

// app 任务侧出队消费（帧泵的"出水管"半边）：JSON 补 \0 后路由，OPUS 交 talk 解码
static void consumeBleEvents() {
    BleFrameEvent ev;
    while (xQueueReceive(bleQ_, &ev, 0) == pdTRUE) {
        if (ev.type == FRAME_TYPE_JSON) {
            char* json = reinterpret_cast<char*>(ev.data);
            json[ev.len] = '\0';  // 入队时按 len+1 分配，这里补终止符
            handleJsonSignal(json);
        } else if (ev.type == FRAME_TYPE_OPUS) {
            if (talk.isActive()) {
                talk.onAudioFrame(ev.data, ev.len);  // talk 会话的 ogg_opus 分片
            } else {
                ESP_LOGW(TAG, "[ble] 非 talk 态收到音频帧 %uB，丢弃", ev.len);
            }
        } else {
            ESP_LOGW(TAG, "[ble] 未知帧 type=0x%02X len=%u", ev.type, ev.len);
        }
        free(ev.data);
    }
}

// ---------------------------------------------------------------- 按键（ADR-016）----
static ButtonHandler btnTop([] { return pwrKeyPressed(); });   // 右上：AXP2101 PKEY
static ButtonHandler btnBottom(PIN_BTN_BOOT);                  // 右下：GPIO0 直读

// 把两颗键的按下/松开/单击/长按接到各自动作（录音状态机的入口全在 Recorder）
static void wireButtons() {
    // 亮屏唤醒挂"按下沿"而非"单击落定"：即按即亮（单击要等 300ms 双击窗口）
    auto wake = [] {
        if (appState.screen() != ScreenState::On) {
            ESP_LOGI(TAG, "[key] wake on press");
        }
        appState.notifyActivity();
        appState.setScreen(ScreenState::On);
    };
    btnTop.onPressDown(wake);
    // 右下按下沿 = 立即开录（ADR-045 v2 定稿，用户拍板"马上就开始录，不允许
    // 丢内容"）：40ms 电气防抖是唯一延迟，开头的字一个不丢。松手不结束——
    // 点一下开始、说完再点一下结束，录音期间手可以离开（挂脖核心场景）。
    // 再按一下的收尾判定（<300ms 录制时长 = 静默丢弃双击误触）在 Recorder::toggle。
    btnBottom.onPressDown([&wake] {
        wake();
        recorder.toggle();
    });

    // 右上单击：亮屏 + 回主页（手表式交互：从卡片/设置/鸭子页一键回消息列表；
    // 已在主页则无可见变化。灭屏交给自动息屏，单击灭屏职责取消——用户拍板）
    btnTop.onSingleClick([] {
        appState.notifyActivity();
        appState.setScreen(ScreenState::On);
        ui.goHome();
    });

    // 右上长按：进入/结束 realtime 对话（M5）
    btnTop.onLongPress([] {
        if (talk.isOff()) {
            if (!gatt.isConnected()) {
                ESP_LOGI(TAG, "[key] top long: BLE 未连接，talk 不可用");
                ui.showNote("请打开手机App");
                return;
            }
            if (!appState.mqttLink()) {
                // 手机侧 MQTT 断（ADR-038）：talk 依赖服务器桥接，提前拦下
                ESP_LOGI(TAG, "[key] top long: 手机侧 MQTT 未连接，talk 不可用");
                ui.showNote("手机没连上服务器");
                return;
            }
            ESP_LOGI(TAG, "[key] top long: talk request");
            talk.start(settings.talkProvider());  // 对话引擎选择权在设备（ADR-035）
        } else {
            ESP_LOGI(TAG, "[key] top long: talk end（本地）");
            talk.stop(true, "device_end");
        }
    });

    // 右下不再注册单击/双击/松开回调：按下沿即录（上文），收尾在"再按一下"
    // 的按下沿。松手宽限/锁定模式已随按住说话退役（ADR-045 v2）
    // talk 期间 toggle→start() 内部拦截并提示"实时对话中，无法录音"。
}

// ---------------------------------------------------------------- 主循环任务 ----
// 主循环任务：消费 BLE 队列 + 扫按键 + 状态机/唤醒/低功耗 tick + UI 刷新。
// 与上行泵分工：这里不碰音频采集/编码/发帧，只管"事件进、状态转、画面出"
static void appTask(void*) {
    for (;;) {
        consumeBleEvents();   // BLE 帧泵的出水口：JSON 路由 / talk 音频转交
        btnTop.loop();
        btnBottom.loop();
        recorder.tick();      // 确认窗 commit
        appState.tick();      // 息屏超时 / talk 连接超时等定时状态机
        gatt.tick();          // 广播看门狗：未连接却不在广播 → 强制重启（今晚实锤的隐身 bug）
        // 摇动 = 开始/结束录音（ADR-040 湿手操作；与右下单击同权，ADR-045）。
        // 录音中摇动 = 停（2026-09-25 用户实测：原条件忽略录音中摇动，
        // "要停的时候摇七八下都没用"——对称开合才是直觉）
        auto wakeEvt = wake.tick();
        if (wakeEvt == Wake::Event::Tap && !appState.isTalking()) {
            recorder.toggle();
        }
        power.tick(gatt.isConnected());  // 挂机深睡判定
        ui.tick();
        // 主循环节拍保持 10ms 不变：摇动判据按 ~11ms 采样标定（4 样本摆），
        // 放宽会破坏阈值。省电靠 IMU Idle 档 + BLE 间隔 + 息屏，不靠任务减速
        vTaskDelay(pdMS_TO_TICKS(10));   // 10ms tick：按键防抖/摇动采样分辨率
    }
}

// ---------------------------------------------------------------- 入口 ----
// 初始化顺序：控制台 → NVS → 设置 → 显示/UI → 状态机 → 音频 → BLE →
//            IMU/唤醒/低功耗 → 按键 → 拉起任务群
extern "C" void app_main() {
    // 原生 USB（USB-Serial-JTAG）在主机打开串口前写入会被丢弃；
    // 等 1.5s 让刷机/复位后的宿主机有时间挂上控制台
    vTaskDelay(pdMS_TO_TICKS(1500));

    printf("gaga ai fw-idf %s\n", FW_VERSION);  // 版本行：验收锚点，纯文本无前缀

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_BT);
    printf("mac: %02X:%02X:%02X:%02X:%02X:%02X\n",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    // NVS：BLE 校准数据 + 用户设置（擦除重建一次兜底）
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[boot] nvs_flash_init: %s", esp_err_to_name(err));
    }

    // ---- 装配上下文：模块先互见，再逐个 begin ----
    ctx.app      = &appState;
    ctx.log      = &msgLog;
    ctx.settings = &settings;
    ctx.ui       = &ui;
    ctx.audio    = &audio;
    ctx.link     = nullptr;  // 装配分支后决定（BLE / WiFi）
    ctx.talk     = &talk;
    ctx.enc      = &opusEnc;
    ctx.rec      = &recorder;
    ctx.pump     = &pump;
    ctx.wake     = &wake;
    ctx.power    = &power;
    ctx.kws      = &kws;
    ctx.link     = &gatt;   // 默认 BLE 链路：泵任务高优先级可能先于链路选择
                            // 运行（2026-09-26 启动崩溃根因），此处保证非空；
                            // 家模式分支随后按需覆盖

    ESP_LOGI(TAG, "[heap] 起点 内部=%luB",
             (unsigned long)esp_get_free_internal_heap_size());
    bleQ_ = xQueueCreate(32, sizeof(BleFrameEvent));

    // 显示 + UI（设置先载入：亮度/息屏时长开机即生效）
    settings.load();
    if (displayStart() != nullptr) {
        displaySetBrightness(settings.brightness());
        ui.setSettings(&settings);
        ui.onApplySetting([](Ui::SettingKey key, int value) {
            switch (key) {
            case Ui::SettingKey::Volume:         audio.setVolume(value); break;
            case Ui::SettingKey::Brightness:     displaySetBrightness(value); break;
            case Ui::SettingKey::ScreenTimeout:
                appState.setScreenTimeoutMs(static_cast<uint32_t>(value)); break;
            case Ui::SettingKey::Beep:           audio.setBeepEnabled(value != 0); break;
            case Ui::SettingKey::Kws:            kws.setEnabled(value != 0); break;
            case Ui::SettingKey::Subtitles:      break;  // 纯 UI 行为（Ui 内部看 settings_），无硬件动作
            case Ui::SettingKey::Shake:          break;  // Wake 每 tick 直读 settings_，无需推送
            case Ui::SettingKey::Lift:           break;  // 同上，直读
            case Ui::SettingKey::AutoRotate:
                // 关自动转向时按手动方向立即落位；开则交给自动规则
                if (value == 0) {
                    displaySetRotation(settings.screenFlip() ? 180 : 0);
                }
                break;
            case Ui::SettingKey::ScreenFlip:
                // 手动方向只在自动转向关闭时落地
                if (!settings.autoRotateEnabled()) {
                    displaySetRotation(value != 0 ? 180 : 0);
                }
                break;
            case Ui::SettingKey::TalkProvider:   break;  // provider 选择由 server 桥消费（信令下发），固件无动作
            }
        });
        ui.begin(&appState, &msgLog);
        // 开机按已存的手动方向落位（自动转向关闭时）
        if (!settings.autoRotateEnabled()) {
            displaySetRotation(settings.screenFlip() ? 180 : 0);
        }
    }

    appState.begin();
    appState.setScreenTimeoutMs(settings.screenTimeoutMs());

    // 音频（BSP codec + 编码器 + 上行泵 + 录音语义层）
    audio.begin();
    audio.setVolume(settings.volume());
    audio.setBeepEnabled(settings.beepEnabled());
    if (!opusEnc.begin()) {
        ESP_LOGE(TAG, "[boot] opus 编码器初始化失败");
    }
    recorder.begin(&ctx);
    pump.begin(&ctx);

    // ---- 链路选择（ADR-039 家/外出模式，二选一互斥）----
    Link* activeLink = nullptr;
    const bool homeMode = settings.wifiEnabled() && settings.wifiConfigured();
    auto wireLinkEvents = [&](Link& link) {
        link.onFrame(onBleFrame);  // 帧泵进水口：host 任务只入队
        link.onConnection([&](bool up) {
            ui.setBleConnected(up);  // 状态栏圆点 = 链路状态
            if (up) {
                char buf[160];
                const char* nm = settings.devName();
                if (nm[0] != '\0')
                    snprintf(buf, sizeof(buf),
                             "{\"type\":\"hello\",\"device\":\"%s\",\"name\":\"%s\",\"fw\":\"%s\"}",
                             DEVICE_ID, nm, FW_VERSION);
                else
                    snprintf(buf, sizeof(buf),
                             "{\"type\":\"hello\",\"device\":\"%s\",\"fw\":\"%s\"}",
                             DEVICE_ID, FW_VERSION);
                link.sendJson(buf);
            } else if (talk.isActive()) {
                ESP_LOGW(TAG, "[talk] 链路断连，会话本地结束");
                talk.stop(false, "link_lost");
            }
        });
    };
    if (false) {
        // 家模式：暂不可用（WiFi 栈已从构建移除——内存不足，ADR-039 备注）
    } else {
        gatt.begin();
        wireLinkEvents(gatt);
        ctx.link = &gatt;
        ESP_LOGI(TAG, "[net] 外出模式：BLE 转发（name=%s）", gatt.deviceName());
    }

    // talk 会话（回调 → UI 提示）——绑定当前链路
    talk.begin(ctx.link, &appState, &audio);
    talk.onText([](bool isReply, const char* text, bool final) {
        ui.setTalkText(isReply, text, final);  // talk_asr/talk_reply 字幕上屏
    });
    talk.onEnd([](const char* reason) {
        char buf[64];
        snprintf(buf, sizeof(buf), "对话结束（%s）", reason);
        ui.showNote(buf);
    });
    talk.onError([](const char* code, const char* msg) {
        char buf[96];
        snprintf(buf, sizeof(buf), "对话错误 %s", code);
        ui.showNote(buf);
    });


    // 时钟 / 按键
    rtcInit();
    if (pwrKeyInit()) {
        btnTop.begin();
    }
    btnBottom.begin();
    wireButtons();

    // 唤醒三件套（ADR-037）：IMU 硬件双击 + 抬手姿态 + 关键词（默认关）
    imu.begin(bsp_i2c_get_handle());
    wake.begin(&imu, &appState, &settings);  // 设置直读：摇动录音/抬手亮屏开关
    // 自动转向：持握角变化 → 转屏 + 左右箭头滚动动画
    wake.onRotation([](int deg) { ui.applyRotation(deg); });
    power.begin(&appState);
    // 息屏省电联动（2026-09-26）：IMU 降到 Idle 档（94Hz 无陀螺，传感器电流
    // 约减半）；亮屏恢复 Active。抬手/摇动检测在 94Hz 下照常工作（摆检测
    // 粒度 11ms→10.7ms，阈值无感）
    appState.onScreenChange([](ScreenState s) { imu.setPowerProfile(s == ScreenState::On); });
    // 亮屏 → 全量重刷（ADR-054）：熄屏期间 Ui::tick 被闸（黑屏不渲染，
    // 省掉"正在回复…"动效每 500ms 对睡面板的全量重绘），亮屏瞬间对齐
    appState.onScreenChange([](ScreenState s) { if (s == ScreenState::On) ui.fullRefresh(); });
    kws.begin(&ctx);
    // ⚠️ 暂不开机自动恢复 KWS：esp_srmodel_init 的 mmap 路径真机仍崩
    // （boot loop 引信，ADR-037 备注），稳定前仅手动开（'W'/设置页）。
    // 崩溃修复后恢复这行：if (settings.kwsEnabled()) kws.setEnabled(true);

    // 任务群（栈全在 PSRAM：内部 RAM 紧张）
    // 上行泵在 pump.begin 已拉起（core1 最高优先级：音频不能断流）
    taskCreatePsram(appTask, "gaga_app", 8192, nullptr, 4, nullptr, tskNO_AFFINITY);
    serial.begin(&ctx);  // 串口命令：优先级最低，纯调试

    ESP_LOGI(TAG, "[boot] ready（内部堆剩余 %luB / 低点 %luB，PSRAM 剩余 %luB）",
             static_cast<unsigned long>(esp_get_free_internal_heap_size()),
             static_cast<unsigned long>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned long>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
    // 开机自检：上电→冒烟滴→断电回零耗（作业 FIFO，ADR-027）
    audio.requestOpen(AudioPipe::RATE_REC);
    audio.event(AudioPipe::SndEv::Listen);  // 开机一声——鸭子打招呼
    audio.requestClose();
}
