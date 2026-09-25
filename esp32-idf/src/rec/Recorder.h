#pragma once

#include <cstdint>

// 按住说话语义层（原 main.cpp 录音半边，重构搬出）：
//   按下沿 → 立即进录音态+红点 UI（即时反馈）→ 音频上电 →"滴"
//   300ms 确认窗 → 过窗才算认真的（发 rec_start，此前松手=误触静默撤销）
//   松手 → 宽限窗（500ms 内重新咬合=无缝续录，ADR-025 握力抖动免疫）
//         宽限耗尽 → 真松手：占位卡 + rec_stop + "滴滴" + 断电（ADR-027）
//   锁定模式（串口 'l'）：松手完全不算数，再按一下才结束
// 上行泵（mic→Opus→BLE）在 UplinkPump，本类只管语义、信令和次序。
namespace gaga {

class AppContext;

class Recorder {
public:
    // 松手宽限默认（串口 'w' 循环 0/250/500/1000/2000ms）
    static constexpr uint32_t DEFAULT_GRACE_MS = 500;

    void begin(AppContext* ctx);

    // ---- 按键事件入口（main 的 wireButtons 转发进来）----
    void pressDown();   // 右下按下沿（含录音中再按：锁定收尾/宽限续录）
    void releaseUp();   // 右下松开（确认窗内=误触；宽限窗逻辑在这）
    void toggle();      // 串口 'r'：空闲开录 / 录音中收尾

    // app 主循环 tick：确认窗到点 commit + 宽限窗耗尽收尾
    void tick();

    // 串口调参
    void cycleGrace();     // 'w'：宽限档位循环
    void toggleLatch();    // 'l'：锁定模式
    bool latchMode() const { return latchMode_; }
    uint32_t graceMs() const { return graceMs_; }

private:
    void start();
    void cancel();   // 误触：静默撤销，零信令
    void finish();   // 正常收尾：占位卡→rec_stop→滴滴→断电（次序不能乱）

    AppContext* ctx_ = nullptr;

    uint32_t graceMs_      = DEFAULT_GRACE_MS;  // 松手宽限
    bool     latchMode_    = false;             // 锁定模式：松手不算数
    bool     releasePending_ = false;           // 宽限窗内（等重新咬合 or 到点收尾）
    uint32_t releaseAtMs_  = 0;                 // 宽限窗到点时刻
    uint32_t commitAtMs_   = 0;                 // 确认窗到点时刻
};

}  // namespace gaga
