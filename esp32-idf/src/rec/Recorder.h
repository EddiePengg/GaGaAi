#pragma once

#include <cstdint>

// 录音语义层（原 main.cpp 录音半边，重构搬出）。
// 交互定稿（2026-09-26 ADR-045 v2，用户拍板"马上就开始录，不允许丢内容"）：
//   toggle()（右下按下沿 / 摇动 / 串口 'r'，三者同权任一可开合）：
//     空闲 → 立即进录音态 + 红点 UI → 音频上电 →"嘎"（第一帧麦克风数据到手）
//     → **开录零窗口零延迟**（电气防抖 40ms 是全部延迟，开头一个字都不丢）
//     录音中再触发 → 收尾判定：
//       rec_start 未发（<300ms 确认窗，泵缓冲着没上行）→ 静默撤销零信令
//       —— 急促双击/误触双拍在这里被丢弃，净效果 = 什么都没发生
//       已过确认窗 → 正常 finish（rec_stop + 占位卡 + 嘎嘎）
//   松手不结束：点一下开始、说完再点一下结束，录音期间手可以离开（挂脖
//   核心场景：骑车/吃东西时按下开录，手放回去说话）。按住说话的松手宽限/
//   锁定模式（v0.x 遗产）已随 ADR-045 退役删除。
// 上行泵（mic→Opus→BLE）在 UplinkPump，本类只管语义、信令和次序。
namespace gaga {

class AppContext;

class Recorder {
public:
    void begin(AppContext* ctx);

    // ---- 录音事件入口 ----
    void toggle();   // 开/关唯一入口（右下按下沿、摇动、串口 'r' 共用）

    // app 主循环 tick：确认窗到点 commit（rec_start + 缓冲整体冲出）
    void tick();

private:
    void start();
    void cancel();   // 误触：静默撤销，零信令
    void finish();   // 正常收尾：占位卡→rec_stop→嘎嘎→断电（次序不能乱）

    AppContext* ctx_ = nullptr;

    uint32_t commitAtMs_ = 0;   // 确认窗到点时刻
};

}  // namespace gaga
