#pragma once

#include <cstdint>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_afe_sr_iface.h"   // AFE 接口（C 链接；前向声明会和命名空间冲突）

// 关键词唤醒（ADR-037 第三唤醒，esp-sr WakeNet9）：
//   "Hi,乐鑫"（你好乐行，官方 wn9_hilexin 模型，经 idf_component.yml 打包进
//   flash "model" 分区）→ 亮屏 + 提示。用户拍板"不想要麦克风一直开着"——
//   做成设置开关（默认关）：开启时才 mic 常开本地推理（不上传），关=零功耗零监听。
//   录音/talk 进行时 KWS 让路（读 mic 前查 AppState，抢到帧的竞态窗口 ~20ms，已知项）。
// 内存：AFE 走 MORE_PSRAM（内部 RAM 预算紧）；WiFi 模式下拒绝开启（28.6KB 盘子
//   装不下两者）。9/25 三坑的绕法：内置模型免 srmodel 手写分区表项之外的加载、
//   崩溃则由 TWDT_PANIC 打印肇事栈（取证体系已就位）。
namespace gaga {

class AppContext;

class KeywordWake {
public:
    void begin(AppContext* ctx);

    // 设置开关：开 = 音频前端上电 + 建 AFE + 拉起 KWS 任务（mic 常开）；
    // 关 = 停任务 + AFE 销毁 + 音频前端断电（若录音没在用）
    bool setEnabled(bool on);
    bool enabled() const { return enabled_; }

private:
    static void taskEntry(void* arg);
    void run();

    AppContext* ctx_ = nullptr;
    volatile bool enabled_ = false;
    volatile bool taskStop_ = false;
    esp_afe_sr_data_t*  afe_ = nullptr;
    const esp_afe_sr_iface_t* iface_ = nullptr;
    TaskHandle_t task_ = nullptr;
};

}  // namespace gaga
