#include "debug/SerialCmd.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <unistd.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "bsp/esp-bsp.h"
#include "lvgl.h"

#include "AppContext.h"
#include "audio/AudioPipe.h"
#include "audio/UplinkPump.h"
#include "ble/GattServer.h"
#include "compat.h"
#include "input/PwrKey.h"
#include "motion/Wake.h"
#include "power/Power.h"
#include "rec/Recorder.h"
#include "state/AppState.h"
#include "state/MsgLog.h"
#include "state/Settings.h"
#include "talk/TalkSession.h"
#include "ui/Display.h"
#include "ui/Ui.h"
#include "voice/KeywordWake.h"

extern const lv_font_t gaga_font_cjk_16;  // 自生子集字体（全局作用域符号）

namespace gaga {

static const char* TAG = "gaga.serial";

void SerialCmd::begin(AppContext* ctx) {
    ctx_ = ctx;
    taskCreatePsram(taskEntry, "gaga_serial", 8192, this, 1, nullptr, tskNO_AFFINITY);
}

void SerialCmd::taskEntry(void* arg) {
    static_cast<SerialCmd*>(arg)->run();
}

// 串口命令泵：阻塞读 USB 控制台 stdin，一键一事（键位表见头注释）
void SerialCmd::run() {
    for (;;) {
        uint8_t c = 0;
        const int n = read(STDIN_FILENO, &c, 1);  // USB 控制台 stdin（阻塞）
        if (n <= 0) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        handleKey(c);
    }
}

void SerialCmd::handleKey(uint8_t c) {
    AppContext& k = *ctx_;
    switch (c) {
    // ---------------- 【用户调试】 ----------------
    case 'r':  // 模拟按住说话：空闲=开录，录音中=收尾（不走松手宽限）
        k.rec->toggle();
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
        k.app->setScreen(k.app->screen() == ScreenState::On ? ScreenState::Off
                                                            : ScreenState::On);
        k.app->notifyActivity();
        break;
    case 'w':  // 松手宽限循环
        k.rec->cycleGrace();
        break;
    case 'l':  // 锁定模式开关
        k.rec->toggleLatch();
        break;
    case 'S': {  // 设置页开/关（触摸之外的入口）
        static bool open = false;
        open = !open;
        if (k.app->screen() == ScreenState::Off) k.app->setScreen(ScreenState::On);
        k.app->notifyActivity();
        if (open) k.ui->openSettings(); else k.ui->closeSettings();
        break;
    }
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
    case 'I':  // IMU 敲击引擎寄存器 dump
        ctx_->wake->dumpTapDebug();
        break;
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
    case 'R': {  // 手动 0↔180 反转（自动转向的验证/兜底路径）
        const int next = (displayGetRotation() == 180) ? 0 : 180;
        displaySetRotation(next);
        ESP_LOGI(TAG, "[dbg] 手动旋转 → %d°", next);
        break;
    }
    case 'Z':  // 立即深睡（验证 BOOT 键唤醒路径）
        k.power->sleepNow();
        break;
    case 'W': {  // 语音唤醒开关切换（"Hi,乐鑫"，开 = mic 常开本地推理）
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
        if (k.app->screen() == ScreenState::Off) k.app->setScreen(ScreenState::On);
        k.app->notifyActivity();
        if (k.ui->companionOpen()) k.ui->closeCompanion(); else k.ui->openCompanion();
        break;
    case 'F':  // 整帧转储（scripts/screenshot.py 收，像 ADB 截屏）
        frameDump();
        break;
    case 'Q':  // 设置页开/关（下拉手势已与设置合并，此为无触摸验证路径）
        if (k.app->screen() == ScreenState::Off) k.app->setScreen(ScreenState::On);
        k.app->notifyActivity();
        if (k.ui->settingsOpen()) k.ui->closeSettings(); else k.ui->openSettings();
        break;

    // ---------------- 【验收】 ----------------
    case 'y': {  // 注入一对完整问答卡（不需要服务端）
        k.log->addSending();
        k.log->fillAsk("这是一条测试消息，看看详情页排版");
        k.log->fillReply("这是模拟回复文本🦆😀带 emoji，用来验收过滤和显示效果");
        k.ui->onLogChanged();
        break;
    }
    case 'u': {  // 注入一张"正在回复"的等待卡
        k.log->addSending();
        k.log->fillAsk("这是一条还在等回复的消息");
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
    case 'h':  // 内存体检（内部/最大连续块/PSRAM/任务栈水位）
        heapReport();
        break;
    default:
        break;
    }
}

// 'W'：整帧 RGB565 转储——LVGL 离线渲染当前屏，"FRAME <字节数>\n"头 + 裸数据。
// Mac 端 scripts/screenshot.py 收并转 PNG（真·截屏，视觉 bug 调试的眼睛）。
// 栈安全：932B 行缓冲用 static；总传输 ~5s@921600（阻塞串口任务无妨，它最低优先级）。
void SerialCmd::frameDump() {
    if (!displayLock(2000)) {
        ESP_LOGW(TAG, "[frame] lvgl lock 超时");
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
    // 断言那课的延伸观测）
    const UBaseType_t n = uxTaskGetNumberOfTasks();
    static TaskStatus_t status[24];
    const UBaseType_t got = uxTaskGetSystemState(status, sizeof(status) / sizeof(status[0]),
                                                 nullptr);
    if (got == 0) return;
    // 选择排序打前 8 名水位最低的（任务数 ~10，简单排序够用）
    bool used[24] = {};
    for (int i = 0; i < 8 && i < static_cast<int>(got); i++) {
        int best = -1;
        for (UBaseType_t j = 0; j < got; j++) {
            if (used[j]) continue;
            if (best < 0 || status[j].usStackHighWaterMark <
                                status[best].usStackHighWaterMark) {
                best = static_cast<int>(j);
            }
        }
        if (best < 0) break;
        used[best] = true;
        printf("[mem] 栈水位 %-14s %5uB%s\n", status[best].pcTaskName,
               static_cast<unsigned>(status[best].usStackHighWaterMark),
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
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
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
        GPIO_NUM_16, GPIO_NUM_9, GPIO_NUM_45, GPIO_NUM_8, GPIO_NUM_10,
    };
    static const char* kNames[] = { "MCLK16", "BCLK9", "WS45", "DOUT8", "DIN10" };
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

// 'p'/'P'：LVGL 像素快照——离线渲染当前屏到 RGB565，统计 + ASCII 画。
// 我不能拍照，这是机器可验证的"屏幕到底渲染了什么"证据（布局回归都用它）
void SerialCmd::snapshotDump() {
    if (!displayLock(2000)) {
        ESP_LOGW(TAG, "[snap] lvgl lock 超时");
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
void SerialCmd::colorTestSnapshot() {
    if (!displayLock(2000)) {
        ESP_LOGW(TAG, "[ctest] lvgl lock 超时");
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
