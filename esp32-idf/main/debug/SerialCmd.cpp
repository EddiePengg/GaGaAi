#include "debug/SerialCmd.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <unistd.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "cJSON.h"
#include "lvgl.h"

#include "AppContext.h"
#include "audio/AudioPipe.h"
#include "audio/UplinkPump.h"
#include "ble/GattServer.h"
#include "boards/common/Board.h"
#include "boards/common/board_selected.h"   // GAGA_PIN_I2S_* 引脚权威
#include "compat.h"
#include "input/PwrKey.h"
#include "motion/Wake.h"
#include "power/Power.h"
#include "rec/Recorder.h"
#include "state/AppState.h"
#include "state/MsgLog.h"
#include "state/Settings.h"
#include "talk/TalkSession.h"
#include "net/LinkManager.h"
#include "ui/Display.h"
#include "ui/Ui.h"
#include "voice/KeywordWake.h"

extern const lv_font_t gaga_font_cjk_16;  // 自生子集字体（全局作用域符号）

namespace gaga {

static const char* TAG = "gaga.serial";

static bool s_logVerbose = false;  // 'G'：全局日志 DEBUG 开关（排障用）

// 渲染类命令（'F'/'p'/'P'/'c'）的 PSRAM 守卫（2026-09-28 晚）：全屏快照要
// ~435KB 连续 PSRAM；不够就直接拒绝（以前看内部堆是错的方向——三次栈溢出
// 实锤瓶颈是串口任务 4KB 栈装不下渲染调用链，与堆无关，根治 = 换 LVGL 任务执行）。
static bool renderGuarded() {
    const uint32_t psFree = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    if (psFree < 500000) {
        printf("[dbg] PSRAM 空闲 %luB < 500KB，快照命令拒绝执行（分配不出全帧缓冲）\n",
               static_cast<unsigned long>(psFree));
        return true;
    }
    return false;
}

// 渲染类命令统一投递 LVGL 任务执行：串口任务栈 4KB 是硬约束，lv_async_call
// 把活转到 LVGL 任务（大栈 + 原生渲染上下文）。单发互斥：LVGL 任务按队列
// 串行执行，但作业字段只有一份，连着按两次会互相覆盖——pending 期间拒收。
static bool s_lvglJobPending = false;
void SerialCmd::dispatchToLvgl(void (SerialCmd::*body)()) {
    if (s_lvglJobPending) {
        printf("[dbg] 上一个渲染任务还在执行，稍后再按\n");
        return;
    }
    s_lvglJobPending = true;
    static struct { SerialCmd* self; void (SerialCmd::*fn)(); } job;
    job.self = this;
    job.fn = body;
    lv_async_call([](void*) {
        auto* j = &job;
        SerialCmd* s = j->self;
        (s->*(j->fn))();
        s_lvglJobPending = false;
    }, nullptr);
    printf("[dbg] 渲染任务已投递 LVGL 任务执行\n");
}

// 'S'/'Q' 本体（LVGL 任务上下文）：设置页开/关。开关态用 static 存活于多次投递。
void SerialCmd::settingsToggleBody() {
    AppContext& k = *ctx_;
    static bool open = false;
    open = !open;
    k.app->notifyActivity();   // 先戳活动再亮屏（防 appTask 息屏判定竞态）
    if (k.app->screen() == ScreenState::Off) k.app->setScreen(ScreenState::On);
    if (open) k.ui->openSettings(); else k.ui->closeSettings();
}

// 'O' 本体（LVGL 任务上下文）：设置页翻下一页；未开设置时先开。
void SerialCmd::settingsNextBody() {
    AppContext& k = *ctx_;
    if (!k.ui->settingsOpen()) {
        k.app->notifyActivity();
        if (k.app->screen() == ScreenState::Off) k.app->setScreen(ScreenState::On);
        k.ui->openSettings();
        return;
    }
    k.ui->flipSettingsPage();
}

void SerialCmd::begin(AppContext* ctx) {
    ctx_ = ctx;
    // 栈必须在内部 RAM：handleLine 的 settings.save() 会写 NVS（flash），
    // flash 写入瞬间要冻结缓存——若本任务栈在 PSRAM 会直接断言重启
    // （esp_cache_utils.c:96 s_task_stack_is_sane_when_cache_frozen，
    // 2026-09-27 'w' 命令实锤）。栈 8KB→4KB（纯调试命令，足够）。
    // 栈 4096→6144→8192（2026-09-29 晚二进宫）：'Q'/'r' 同步渲染 4096 必溢；
    // 6144 上 'S' 建页链被金丝雀实锤处决。S/Q/O 已投 LVGL 任务（见上），
    // 8192 是给残留同步路径（y/u/R/C/r）对齐 appTask 的深度余量。
    xTaskCreate(taskEntry, "gaga_serial", 8192, this, 1, nullptr);
}

void SerialCmd::taskEntry(void* arg) {
    static_cast<SerialCmd*>(arg)->run();
}

// 串口命令泵：阻塞读 USB 控制台 stdin，一键一事（键位表见头注释）；
// 'w' 进 WiFi 配置行模式后，字符累积到回车提交（handleLine），ESC 取消
void SerialCmd::run() {
    for (;;) {
        uint8_t c = 0;
        const int n = read(STDIN_FILENO, &c, 1);  // USB 控制台 stdin（阻塞）
        if (n <= 0) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        if (lineMode_ != 0) {
            if (c == 27) {                 // ESC 取消行模式
                lineMode_ = 0;
                printf("\n[dbg] 已取消\n");
                continue;
            }
            if (c == '\n' || c == '\r') {
                if (lineLen_ > 0) {
                    lineBuf_[lineLen_] = '\0';
                    handleLine(lineBuf_);
                }
                lineMode_ = 0;
                lineLen_ = 0;
                continue;
            }
            if (c >= 0x20 && lineLen_ < static_cast<int>(sizeof(lineBuf_)) - 1) {
                lineBuf_[lineLen_++] = static_cast<char>(c);
            }
            continue;
        }
        if (c >= 0x21 && c <= 0x7e) {
            ESP_LOGI(TAG, "[cmd] '%c'", c);  // 留痕：崩溃现场可回放最后执行了什么
        }
        handleKey(c);
    }
}

// 'w'/'H' 提交：'w' = WiFi+broker 全量（ssid/pass 必填）；'H' = 仅 broker
//（host/port/mqtt_user/mqtt_pass，不碰 WiFi 凭证——2026-09-28 加：换 broker
// 不该要求重输 WiFi 密码）→ NVS
void SerialCmd::handleLine(const char* line) {
    cJSON* root = cJSON_Parse(line);
    if (root == nullptr) {
        printf("[dbg] JSON 解析失败：%s\n", line);
        return;
    }
    const cJSON* host = cJSON_GetObjectItem(root, "host");
    const cJSON* port = cJSON_GetObjectItem(root, "port");
    const cJSON* muser = cJSON_GetObjectItem(root, "mqtt_user");
    const cJSON* mpass = cJSON_GetObjectItem(root, "mqtt_pass");
    if (lineMode_ == 2) {  // 'H'：仅 broker
        if (cJSON_IsString(host) && host->valuestring[0] != '\0') {
            ctx_->settings->setMqtt(host->valuestring,
                                    cJSON_IsNumber(port) ? port->valueint : 1883);
        }
        if (cJSON_IsString(muser)) {
            ctx_->settings->setMqttAuth(muser->valuestring,
                                        cJSON_IsString(mpass) ? mpass->valuestring : "");
        }
        ctx_->settings->save();
        printf("[dbg] broker 已存：%s:%d user=%s（'N' 切链路生效）\n",
               ctx_->settings->mqttHost(), ctx_->settings->mqttPort(),
               ctx_->settings->mqttUser()[0] != '\0' ? ctx_->settings->mqttUser() : "-");
        cJSON_Delete(root);
        return;
    }
    const cJSON* ssid = cJSON_GetObjectItem(root, "ssid");
    const cJSON* pass = cJSON_GetObjectItem(root, "pass");
    if (cJSON_IsString(ssid) && ssid->valuestring[0] != '\0') {
        ctx_->settings->setWifi(cJSON_IsString(ssid) ? ssid->valuestring : "",
                                cJSON_IsString(pass) ? pass->valuestring : "");
        if (cJSON_IsString(host) && host->valuestring[0] != '\0') {
            ctx_->settings->setMqtt(host->valuestring,
                                    cJSON_IsNumber(port) ? port->valueint : 1883);
        }
        if (cJSON_IsString(muser)) {
            ctx_->settings->setMqttAuth(muser->valuestring,
                                        cJSON_IsString(mpass) ? mpass->valuestring : "");
        }
        ctx_->settings->save();
        printf("[dbg] WiFi 已存：ssid=%s host=%s:%d user=%s（'N' 切强制WiFi 生效）\n",
               ctx_->settings->wifiSsid(), ctx_->settings->mqttHost(),
               ctx_->settings->mqttPort(),
               ctx_->settings->mqttUser()[0] != '\0' ? ctx_->settings->mqttUser() : "-");
    } else {
        printf("[dbg] 无效：需要非空 ssid\n");
    }
    cJSON_Delete(root);
}

void SerialCmd::handleKey(uint8_t c) {
    AppContext& k = *ctx_;
    switch (c) {
    // ---------------- 【用户调试】 ----------------
    case 'r':  // 模拟开/关录音（与右下按下沿、摇动同权）：空闲=开录，录音中=收尾判定
        // （2026-09-29 晚回滚异步化实验：恢复同步直调，与 wave-1 精确版一致——
        // 异步版改变时序后用户实测按键收尾反而重启。串口 'r' 为诊断路径。）
        k.rec->toggle();
        break;
    case 'N': {  // 链路模式循环 Auto → 强制BLE → 强制WiFi（家模式互斥切换的手动入口）
        if (k.net == nullptr) break;
        auto m = k.net->mode();
        m = (m == LinkManager::Mode::Auto)      ? LinkManager::Mode::ForceBle
          : (m == LinkManager::Mode::ForceBle)  ? LinkManager::Mode::ForceWifi
          :                                       LinkManager::Mode::Auto;
        k.net->setMode(m);
        ESP_LOGI(TAG, "[dbg] 链路模式 → %s（当前活动=%s，连接=%d）",
                 k.net->modeName(), k.net->activeName(), k.net->isConnected() ? 1 : 0);
        break;
    }
    case 'w':  // WiFi 凭证+broker 行配置：下一段输入是 JSON 行（ssid/pass/host/port）
        lineMode_ = 1;
        lineLen_  = 0;
        printf("\n[dbg] 输入 WiFi 配置 JSON，回车提交（例："
               "{\"ssid\":\"Home\",\"pass\":\"pwd\",\"host\":\"192.168.1.10\",\"port\":1883}）：\n");
        break;
    case 'H':  // 仅 broker 行配置（不动 WiFi 凭证）：下一段输入 {"host","port","mqtt_user","mqtt_pass"}
        lineMode_ = 2;
        lineLen_  = 0;
        printf("\n[dbg] 输入 broker 配置 JSON，回车提交（例："
               "{\"host\":\"192.168.1.10\",\"port\":1883,\"mqtt_user\":\"u\",\"mqtt_pass\":\"p\"}）：\n");
        break;
    case 'G':  // 全局日志 → DEBUG（排障：看 WiFi init 内部哪步失败；再按恢复 INFO）
        s_logVerbose = !s_logVerbose;
        esp_log_level_set("*", s_logVerbose ? ESP_LOG_DEBUG : ESP_LOG_INFO);
        ESP_LOGI(TAG, "[dbg] 全局日志 → %s", s_logVerbose ? "DEBUG" : "INFO");
        break;
    case 't':  // 模拟右上长按：talk 开始 / 结束切换
        if (k.talk->isOff()) k.talk->start(); else k.talk->stop(true, "serial_end");
        break;
    case 'm':  // 下混声道循环：0=左(MIC1) 1=右(MIC2) 2=平均
        k.pump->cycleMicChannel();
        break;
    case 'b':  // 提示音"滴"一声（播音通路冒烟）
        k.audio->beep(1);
        break;
    case 's':  // 亮/息屏切换（测息屏唤醒与超时）
        k.app->notifyActivity();   // 先戳活动再亮屏：否则 appTask 的息屏判定
        k.app->setScreen(k.app->screen() == ScreenState::On ? ScreenState::Off
                                                            : ScreenState::On);
        break;
    case 'S':   // 设置页开/关（触摸之外的入口）——建页链太深，投 LVGL 任务执行
    case 'Q':   // 设置页开/关（同 'S'，历史双键位）
        dispatchToLvgl(&SerialCmd::settingsToggleBody);
        break;
    case 'O':   // 设置页翻下一页（未开设置时先开）——同上投 LVGL 任务
        dispatchToLvgl(&SerialCmd::settingsNextBody);
        break;
    case 'V': {  // 音量循环 0/30/60/85/100
        static const int kSteps[] = {0, 30, 60, 85, 100};
        int next = kSteps[0];
        for (int i = 0; i < 4; i++) {
            if (kSteps[i] == k.settings->volume()) { next = kSteps[i + 1]; break; }
        }
        k.settings->setVolume(next);
        k.settings->save();
        k.audio->setVolume(next);
        ESP_LOGI(TAG, "[dbg] 音量 → %d%%", next);
        break;
    }
    case 'L': {  // 亮度循环 10/30/60/100
        static const int kSteps[] = {10, 30, 60, 100};
        int next = kSteps[0];
        for (int i = 0; i < 3; i++) {
            if (kSteps[i] == k.settings->brightness()) { next = kSteps[i + 1]; break; }
        }
        k.settings->setBrightness(next);
        k.settings->save();
        displaySetBrightness(next);
        ESP_LOGI(TAG, "[dbg] 亮度 → %d%%", next);
        break;
    }
    case 'A': {  // IMU 原始加速度流开关（摇动阈值离线调参：抓日志分析）
        const bool on = !k.wake->streamAccel();
        k.wake->setStreamAccel(on);
        ESP_LOGI(TAG, "[A] stream → %s（10ms/行：t,模长,偏离,xyz；触发只记 would-fire）",
                 on ? "ON" : "OFF");
        break;
    }
    case 'M':  // IMU 姿态 + 唤醒状态机现场（抬手调参的眼睛）
        imuDump();
        break;
    case 'K':  // 抬手 Z 轴极性翻转（设备焊装方向兜底）
        k.wake->flipZSign();
        ESP_LOGI(TAG, "[dbg] 抬手 Z 极性已翻转");
        break;
    case 'k': {  // 语音唤醒开关（直接 toggle + 落盘；内存/KWS 成本测量专用通道）
        if (k.kws == nullptr || k.settings == nullptr) break;
        const bool on = !k.kws->enabled();
        if (k.kws->setEnabled(on)) {
            k.settings->setKwsEnabled(on);
            k.settings->save();
        }
        ESP_LOGI(TAG, "[dbg] KWS → %s（内部剩余=%u）", on ? "开" : "关",
                 static_cast<unsigned>(esp_get_free_internal_heap_size()));
        break;
    }
    case 'R': {  // 手动 0↔180 反转（自动转向的验证/兜底路径）
        const int next = (displayGetRotation() == 180) ? 0 : 180;
        displaySetRotation(next);
        ESP_LOGI(TAG, "[dbg] 手动旋转 → %d°", next);
        break;
    }
    case 'Z':  // 立即深睡（验证 BOOT 键唤醒路径）
        k.power->sleepNow();
        break;
    case 'E':  // 故意 abort() 自测 espcoredump 全链路（写盘→重启→espcoredump.py 读尸检）
        ESP_LOGE(TAG, "[dbg] 'E' 触发 panic 自测（espcoredump）——设备将重启");
        vTaskDelay(pdMS_TO_TICKS(200));   // 给日志 200ms 冲出 USB 控制台再死
        abort();
        break;
    case 'W': {  // 语音唤醒开关切换（"你好小智"，开 = mic 常开本地推理）
        const bool on = !k.settings->kwsEnabled();
        k.settings->setKwsEnabled(on);
        k.settings->save();
        k.kws->setEnabled(on);
        ESP_LOGI(TAG, "[dbg] 语音唤醒 → %s", on ? "开" : "关");
        break;
    }
    case 'X': {  // 对话字幕开关切换（关 = talk 纯语音模式，防长文本卡顿）
        const bool on = !k.settings->talkSubtitles();
        k.settings->setTalkSubtitles(on);
        k.settings->save();
        ESP_LOGI(TAG, "[dbg] 对话字幕 → %s", on ? "开" : "关");
        break;
    }
    case 'C':  // 鸭子页开/关（左滑手势的无触摸验证路径，ADR-036）
        k.app->notifyActivity();   // 先戳活动再亮屏（同 's'，防息屏判定竞态）
        if (k.app->screen() == ScreenState::Off) k.app->setScreen(ScreenState::On);
        if (k.ui->companionOpen()) k.ui->closeCompanion(); else k.ui->openCompanion();
        break;
    case 'F':  // 整帧转储（scripts/screenshot.py 收，像 ADB 截屏）
        frameDump();
        break;

    // ---------------- 【验收】 ----------------
    case 'y': {  // 注入一对完整问答卡（不需要服务端）
        k.log->addSending(0);   // rec_seq=0：无真实录音段，走 FIFO 回落配对
        k.log->fillAsk("这是一条测试消息，看看详情页排版", "", 0);
        k.log->fillReply("这是模拟回复文本🦆😀带 emoji，用来验收过滤和显示效果");
        k.ui->onLogChanged();
        break;
    }
    case 'u': {  // 注入一张"正在回复"的等待卡
        k.log->addSending(0);   // rec_seq=0：无真实录音段，走 FIFO 回落配对
        k.log->fillAsk("这是一条还在等回复的消息", "", 0);
        k.ui->onLogChanged();
        break;
    }
    case 'p':  // 屏幕像素快照（纯当前 UI，不放字体探针）
        snapshotUseCjkFont_ = 0;
        snapshotDump();
        break;
    case 'P':  // CJK 子集字体探针快照（中文字形渲染回归）
        snapshotUseCjkFont_ = 1;
        snapshotDump();
        break;
    case 'c':  // 彩底黑块测试（"字形拖黑底"报障专用）
        colorTestSnapshot();
        break;

    // ---------------- 【排障】 ----------------
    case 'd':  // mic 原始采样 dump 开关（L=MIC1/R=MIC2）
        k.pump->toggleMicDebug();
        ESP_LOGI(TAG, "[dbg] mic 原始采样 dump %s", "切换");
        break;
    case 'e':  // dump 音频编解码器寄存器（ES8311/ES7210 配置核对）
        k.audio->dumpCodecRegs();
        break;
    case 'g':  // GPIO 活动探测：I2S 时钟/数据线翻转计数
        pinActivityProbe();
        break;
    case 'i':  // I2C 总线扫描（共享总线上每个地址的 ACK）
        i2cBusScan();
        break;
    case 'I': {  // 堆完整性体检（2026-09-29 幽灵探测器）：全部堆结构校验，
                 // 被踩的块直接打印现形。崩溃复现前按一次最有价值。
        printf("[heap-check] 开始校验全部堆...\n");
        const bool ok = heap_caps_check_integrity_all(true);
        printf("[heap-check] %s\n", ok ? "全部堆完整 ✓" : "发现损坏块 ↑↑↑（ see above ）");
        break;
    }
    case 'n': {  // 自测 notify 通路：发一帧 ping JSON
        const bool ok = k.link->sendJson("{\"type\":\"ping\"}");
        ESP_LOGI(TAG, "[dbg] ping notify sendFrame=%s connected=%d",
                 ok ? "ok" : "FAIL", k.link->isConnected() ? 1 : 0);
        break;
    }
    case 'B': {  // 1s 测量长音（配合 'g' 看播放通路）
        ESP_LOGI(TAG, "[dbg] 长音 1s 播放中…");
        const int n = k.audio->playToneDebug(1000);
        ESP_LOGI(TAG, "[dbg] 长音播放完成，%d 采样", n);
        break;
    }
    case 'D':  // 内部堆全量块表（块级内存分析；trace 取证已撤，2026-09-29）
        printf("\n[heap-dump] 内部堆已分配块：\n");
        heap_caps_dump(MALLOC_CAP_INTERNAL);
        printf("[heap-dump] end\n\n");
        break;
    case 'h':  // 内存体检（内部/最大连续块/PSRAM/任务栈水位）
        heapReport();
        break;
    default:
        break;
    }
}

// 'F'：整帧 RGB565 转储——包装层（守卫+投递）；本体 frameDumpBody 在 LVGL
// 任务执行："FRAME <字节数>\n"头 + 裸数据，Mac 端 scripts/screenshot.py 收并转 PNG。
void SerialCmd::frameDump() {
    if (renderGuarded()) return;
    dispatchToLvgl(&SerialCmd::frameDumpBody);
}

void SerialCmd::frameDumpBody() {   // ⚠️ 仅 LVGL 任务上下文
    if (!displayLock(2000)) {       // 防异步期别的任务也在动屏（防御，通常空闲）
        printf("FRAME_ERR lock\n");
        esp_log_level_set("*", ESP_LOG_INFO);
        return;
    }
    // 转储期间全局静音日志：其他任务的 ESP_LOG 字节会插进二进制像素流
    // （真机实锤：截 PNG 出现斜纹错位 = 日志字节混入）
    esp_log_level_set("*", ESP_LOG_NONE);
    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(NULL);
    lv_draw_buf_t* snap = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_RGB565);
    displayUnlock();
    if (snap == nullptr) {
        printf("FRAME_ERR snap\n");
        return;
    }
    const uint32_t len = snap->data_size;
    printf("FRAME %lu\n", static_cast<unsigned long>(len));
    // 逐行写出（snap->data 非线性？stride 行距处理：按行拷避免 stride 间隙）
    static uint8_t line[932];
    const uint32_t stride = snap->header.stride;
    const uint32_t w2 = snap->header.w * 2;
    for (uint32_t y = 0; y < snap->header.h; y++) {
        memcpy(line, snap->data + y * stride, w2);
        fwrite(line, 1, w2, stdout);
    }
    fflush(stdout);
    lv_draw_buf_destroy(snap);
    esp_log_level_set("*", ESP_LOG_INFO);  // 恢复日志
}

// 'h'：内存体检报告——2026-09-25 刷屏 NO_MEM 事故的观测能力。
// 关键指标是"内部最大连续块"：剩余总量高不等于分配得出（碎片化），
// 屏幕刷屏的手推车（~3.7KB DMA）能不能装下就看它。
void SerialCmd::heapReport() {
    const uint32_t inFree = esp_get_free_internal_heap_size();
    const uint32_t inLargest =
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    // 注意：esp_get_minimum_free_heap_size 在 PSRAM 联合分配下返回合并堆低点
    // （跑出 6.2MB 这种假数），内部低点必须用 caps 版本
    const uint32_t inMin = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL |
                                                           MALLOC_CAP_8BIT);
    const uint32_t psFree = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    const uint32_t psLargest =
        heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    printf("[mem] 内部 剩余=%luB 最大连续=%luB 历史低点=%luB\n",
           static_cast<unsigned long>(inFree),
           static_cast<unsigned long>(inLargest),
           static_cast<unsigned long>(inMin));
    printf("[mem] PSRAM 剩余=%luB 最大连续=%luB\n",
           static_cast<unsigned long>(psFree),
           static_cast<unsigned long>(psLargest));
    // 健康线提示：内部最大连续 < 4KB 时刷屏手推车会开始失败（本次事故的形态）
    if (inLargest < 4096) {
        printf("[mem] ⚠️ 内部最大连续 <4KB：刷屏/音频的 DMA 分配将失败！\n");
    }
    // 任务栈水位：离溢出最近的任务排前面（栈是 PSRAM 的也计入——cache freeze
    // 断言那课的延伸观测）。状态表本身挪 PSRAM（2026-09-29 静态审计：864B 调试件
    // 不该占内部 .bss），24 条×TaskStatus_t(~360B)≈8.6KB PSRAM。
    static TaskStatus_t* status = static_cast<TaskStatus_t*>(heap_caps_malloc(
        24 * sizeof(TaskStatus_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (status == nullptr) {
        printf("[mem] 水位表 PSRAM 分配失败\n");
        return;
    }
    const UBaseType_t got = uxTaskGetSystemState(status, 24, nullptr);
    printf("[mem] 任务总数=%u（数组容量 24，超出部分不打印）\n",
           static_cast<unsigned>(got));
    if (got == 0) return;
    // 全量打印（2026-09-28 晚：倒数 8 名会漏掉水位健康的任务——KWS 栈裁编
    // 要量的正是"6KB 栈实际吃多深"，漏榜等于没量）。
    // ⚠️ got 是系统任务总数（可超数组容量），遍历必须钳到实填条数——
    // 2026-09-28 实锤：任务数超 24 后原循环越界读 status[]/used[]，
    // 串口任务自己的栈被 selection-sort 踩踏，'h' 输出中途暴毙（0~1 行）。
    const UBaseType_t n = got < 24 ? got : 24;
    bool used[24] = {};
    for (UBaseType_t i = 0; i < n; i++) {
        int best = -1;
        for (UBaseType_t j = 0; j < n; j++) {
            if (used[j]) continue;
            if (best < 0 || status[j].usStackHighWaterMark <
                                status[best].usStackHighWaterMark) {
                best = static_cast<int>(j);
            }
        }
        if (best < 0) break;
        used[best] = true;
        printf("[mem] 栈水位 %-14s %5uB 基址=%p%s\n", status[best].pcTaskName,
               static_cast<unsigned>(status[best].usStackHighWaterMark),
               status[best].pxStackBase,
               status[best].usStackHighWaterMark < 512 ? "  ⚠️ <512B！" : "");
    }
}

// 'M'：IMU 现场——姿态三分量 + 唤醒状态机诊断（抬手阈值调参的眼睛）
void SerialCmd::imuDump() {
    Wake* w = ctx_->wake;
    printf("[imu] xyz=(%.2f, %.2f, %.2f)g |a|=%.2f 屏=%s 降频=%s\n",
           w->lastX(), w->lastY(), w->lastZ(),
           sqrtf(w->lastX() * w->lastX() + w->lastY() * w->lastY() +
                 w->lastZ() * w->lastZ()),
           ctx_->app->screen() == ScreenState::On ? "亮" : "息",
           ctx_->power->lowPowerClock() ? "80MHz" : "240MHz");
}

// 'i'：扫 I2C 0x03~0x77，打印应答的地址——看共享总线上到底挂了谁
void SerialCmd::i2cBusScan() {
    i2cLock();
    i2c_master_bus_handle_t bus = Board::instance().i2cBus();
    if (bus == nullptr) {
        i2cUnlock();
        ESP_LOGW(TAG, "[dbg] I2C 总线不可用");
        return;
    }
    printf("[dbg] I2C scan:");
    for (uint8_t addr = 0x03; addr <= 0x77; addr++) {
        if (i2c_master_probe(bus, addr, 30) == ESP_OK) {
            printf(" 0x%02X", addr);
        }
    }
    printf("\n");
    i2cUnlock();
}

// 'g'：GPIO 电平翻转计数（I2S 时钟/数据线活动性检测，采集全零排查用）
void SerialCmd::pinActivityProbe() {
    static constexpr gpio_num_t kPins[] = {
        GAGA_PIN_I2S_MCLK, GAGA_PIN_I2S_BCLK, GAGA_PIN_I2S_WS, GAGA_PIN_I2S_DOUT, GAGA_PIN_I2S_DIN,
    };
    static const char* kNames[] = { "MCLK", "BCLK", "WS", "DOUT", "DIN" };
    printf("[dbg] GPIO 活动探测（300ms 窗口翻转计数）:");
    for (int i = 0; i < 5; i++) {
        int last = gpio_get_level(kPins[i]);
        uint32_t flips = 0;
        const uint32_t start = millis();
        while (millis() - start < 300) {
            const int v = gpio_get_level(kPins[i]);
            if (v != last) { flips++; last = v; }
        }
        printf(" %s=%lu", kNames[i], static_cast<unsigned long>(flips));
    }
    printf("\n");
}

// 'p'/'P'：LVGL 像素快照——包装层（守卫+投递）；本体 snapshotDumpBody 在
// LVGL 任务执行。离线渲染当前屏到 RGB565，统计 + ASCII 画——机器可验证的
// "屏幕到底渲染了什么"证据（布局回归都用它）
void SerialCmd::snapshotDump() {
    if (renderGuarded()) return;
    dispatchToLvgl(&SerialCmd::snapshotDumpBody);
}

void SerialCmd::snapshotDumpBody() {   // ⚠️ 仅 LVGL 任务上下文
    if (!displayLock(2000)) {
        printf("[snap] lvgl lock 超时\n");
        return;
    }
    // 字体自诊：直接查字形描述符（cmap 查找是否生效）
    {
        lv_font_glyph_dsc_t gd;
        memset(&gd, 0, sizeof(gd));
        const bool okA = lv_font_get_glyph_dsc(&gaga_font_cjk_16, &gd, 'A', '\0');
        printf("[snap] CJK 字体查 'A': found=%d box=%ux%u | ", okA, gd.box_w, gd.box_h);
        memset(&gd, 0, sizeof(gd));
        const bool okHan = lv_font_get_glyph_dsc(&gaga_font_cjk_16, &gd, 0x8fde, '\0');
        printf("'连': found=%d box=%ux%u\n", okHan, gd.box_w, gd.box_h);
    }
    // 探针：'P' CJK 子集字体探针（y=+150，与顶部状态行分开）
    lv_obj_t* probe = nullptr;
    if (snapshotUseCjkFont_ != 0) {
        probe = lv_label_create(lv_screen_active());
        lv_obj_set_style_text_font(probe, &gaga_font_cjk_16, LV_PART_MAIN);
        lv_obj_set_style_text_color(probe, lv_color_white(), LV_PART_MAIN);
        lv_obj_align(probe, LV_ALIGN_CENTER, 0, 150);
        lv_label_set_text(probe, "测试ABC连接录音");
    }
    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(NULL);  // 立即重绘（快照取的是当前帧内容）
    lv_draw_buf_t* snap = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_RGB565);
    if (probe) lv_obj_delete(probe);
    displayUnlock();
    if (snap == nullptr) {
        ESP_LOGW(TAG, "[snap] 快照失败（内存不足？）");
        return;
    }
    const uint32_t w = snap->header.w, h = snap->header.h;
    const uint32_t stride = snap->header.stride;  // 字节/行
    printf("[snap] %lux%lu 非黑像素=", static_cast<unsigned long>(w),
           static_cast<unsigned long>(h));
    // 非黑像素统计 + 分带墨迹（8 横带，看内容分布）
    uint32_t nonblack = 0;
    uint32_t bandInk[8] = {0};
    for (uint32_t y = 0; y < h; y++) {
        const uint16_t* row = reinterpret_cast<const uint16_t*>(snap->data + y * stride);
        const int band = static_cast<int>(y * 8 / h);
        for (uint32_t x = 0; x < w; x++) {
            if (row[x] != 0) { nonblack++; bandInk[band]++; }
        }
    }
    printf("%lu（%.2f%%）分带:", static_cast<unsigned long>(nonblack),
           static_cast<double>(nonblack) * 100.0 / (w * h));
    for (int i = 0; i < 8; i++) printf(" %lu", static_cast<unsigned long>(bandInk[i]));
    printf("\n");
    // 32 行 ASCII 亮度画（全屏概览）
    static const char ramp[] = " .:-=+*#%@";
    for (uint32_t ay = 0; ay < 32; ay++) {
        for (uint32_t ax = 0; ax < 64; ax++) {
            const uint32_t x0 = ax * w / 64, x1 = (ax + 1) * w / 64;
            const uint32_t y0 = ay * h / 32, y1 = (ay + 1) * h / 32;
            uint32_t sum = 0, cnt = 0;
            for (uint32_t y = y0; y < y1; y++) {
                const uint16_t* row = reinterpret_cast<const uint16_t*>(snap->data + y * stride);
                for (uint32_t x = x0; x < x1; x++) {
                    const uint16_t c = row[x];
                    sum += ((c >> 11) & 31) * 2 + ((c >> 5) & 63) * 4 + (c & 31);
                    cnt++;
                }
            }
            const uint32_t lum = cnt ? sum / cnt : 0;
            putchar(ramp[lum * 9 / 345]);
        }
        putchar('\n');
    }
    lv_draw_buf_destroy(snap);
}

// 'c'：彩色底测试画面（"字形拖黑底"报障专用；快照字节精确，黑块立刻现形）
// 包装层（守卫+投递）；本体 colorTestSnapshotBody 在 LVGL 任务执行。
void SerialCmd::colorTestSnapshot() {
    if (renderGuarded()) return;
    dispatchToLvgl(&SerialCmd::colorTestSnapshotBody);
}

void SerialCmd::colorTestSnapshotBody() {   // ⚠️ 仅 LVGL 任务上下文
    if (!displayLock(2000)) {
        printf("[ctest] lvgl lock 超时\n");
        return;
    }
    // 绿色背景矩形（中央横带）+ 白字 + 红字 CJK
    lv_obj_t* rect = lv_obj_create(lv_screen_active());
    lv_obj_set_size(rect, 380, 140);
    lv_obj_align(rect, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(rect, lv_palette_main(LV_PALETTE_GREEN), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(rect, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(rect, 0, LV_PART_MAIN);
    lv_obj_t* l1 = lv_label_create(rect);
    lv_obj_set_style_text_font(l1, &gaga_font_cjk_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(l1, lv_color_white(), LV_PART_MAIN);
    lv_label_set_text(l1, "录音中ABC已送达");
    lv_obj_align(l1, LV_ALIGN_CENTER, 0, -24);
    lv_obj_t* l2 = lv_label_create(rect);
    lv_obj_set_style_text_font(l2, &gaga_font_cjk_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(l2, lv_palette_main(LV_PALETTE_RED), LV_PART_MAIN);
    lv_label_set_text(l2, "对话连接中测试");
    lv_obj_align(l2, LV_ALIGN_CENTER, 0, 24);
    lv_refr_now(NULL);
    lv_draw_buf_t* snap = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_RGB565);
    lv_obj_delete(rect);
    displayUnlock();
    if (snap == nullptr) {
        ESP_LOGW(TAG, "[ctest] 快照失败");
        return;
    }
    const uint32_t stride = snap->header.stride;
    uint32_t green = 0, white = 0, red = 0, black = 0, other = 0;
    for (uint32_t y = 163; y < 303; y++) {  // rect 实际区域 x 43..423, y 163..303
        const uint16_t* row = reinterpret_cast<const uint16_t*>(snap->data + y * stride);
        for (uint32_t x = 43; x < 423; x++) {
            const uint16_t c = row[x];
            const uint32_t r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
            if (g > 40 && g > r * 2 && g > b * 2) green++;
            else if (r > 44 && g > 44 && b > 44) white++;
            else if (r > 24 && r > g * 2 && r > b * 2) red++;
            else if (r + g + b < 12) black++;
            else other++;
        }
    }
    printf("[ctest] 绿底=%lu 白字=%lu 红字=%lu 黑块=%lu 其他=%lu\n",
           (unsigned long)green, (unsigned long)white, (unsigned long)red,
           (unsigned long)black, (unsigned long)other);
    lv_draw_buf_destroy(snap);
}

}  // namespace gaga
