#pragma once

#include <cstdint>

// 关键词唤醒（ADR-037 第三唤醒，esp-sr WakeNet9）：
//   "Hi,乐鑫"（你好乐行，官方 wn9_hilexin 模型，flash 内置）→ 亮屏。
// 用户拍板"不想要麦克风一直开着"——做成设置开关（默认关）：开启时才
// mic 常开本地推理（不上传），关=零功耗零监听。录音/talk 进行时 KWS 让路
// （mic 通路归录音泵，KWS 挂起等待）。唤醒动作与按键/双击同路径：亮屏。
// 内存：AFE 走 MORE_PSRAM（内部 RAM 预算紧）；模型在 flash（MODEL_IN_FLASH）。
// 真机参数（阈值/误唤醒率）待实测调优（afe->set_detected_delay 等）。
namespace gaga {

class AppContext;

class KeywordWake {
public:
    void begin(AppContext* ctx);

    // 设置开关：开 = 懒初始化 AFE + 拉起 KWS 任务（mic 常开）；
    // 关 = 停任务 + AFE 销毁 + mic 断电（若录音没在用）
    bool setEnabled(bool on);
    bool enabled() const { return enabled_; }

private:
    AppContext* ctx_ = nullptr;
    volatile bool enabled_ = false;
};

}  // namespace gaga
