#pragma once

#include <cstdint>

// 上行泵（原 main.cpp 的 uplinkTask + flushOneFrame + micReadCycle，重构搬出）：
// 专用大栈任务，一条泵服务两种互斥状态（AppState 保证互斥）：
//   M1 录音：mic @16k 双声道 → 下混 320/20ms → Opus → 帧（确认窗帧入缓冲，
//            commit 后连缓冲整体冲出——按下起零丢失，ADR-026）
//   M5 talk：mic @24k 双声道 → 下混 480 → 2:3 抽取 320 → Opus → 采集即发送
// opus_encode 栈需求大（Arduino 线踩过 loopTask 溢出），泵跑专用 PSRAM 大栈任务。
namespace gaga {

class AppContext;

class UplinkPump {
public:
    // 确认窗（ADR-026）：按下即录，300ms 后才算"认真的"——此前帧入缓冲不上行
    static constexpr uint32_t REC_COMMIT_MS = 300;
    // 确认窗帧缓冲容量：64 × 20ms = 1.28s（覆盖确认窗绰绰有余）
    static constexpr int REC_BUF_PKTS   = 64;
    static constexpr int REC_BUF_PKT_SZ = 64;   // Opus 包实测 8~50B

    void begin(AppContext* ctx);  // 建 PSRAM 大栈任务（绑 core1 最高优先级）

    // ---- Recorder 与泵的握手接口 ----
    void beginSession();   // 按下沿：统计清零、清缓冲、计时开始（泵随后自动跟上）
    void commit();         // 确认窗到点：此后帧直上（此前缓冲在首帧时整体冲出）
    void discard();        // 误触撤销：丢缓冲（泵已停读，无竞争）
    bool committed() const { return committed_; }

    // 串口调参：下混声道（0=左 1=右 2=平均）/ mic 采样 dump 开关
    void cycleMicChannel();
    void toggleMicDebug() { micDebug_ = !micDebug_; }

    // 已成功上行的帧数（Recorder 判定"嘎嘎=已发出"承诺用，0=一条没出去）
    uint32_t framesSent() const { return framesSent_; }

private:
    static void taskEntry(void* arg);
    void run();               // 泵主循环（rec / 收尾统计 / talk / 空闲睡）
    bool micReadCycle(int16_t* monoOut, int framesPerCh);
    void flushOneFrame(const int16_t* mono320, bool isTalk);

    AppContext* ctx_ = nullptr;

    int      micChanSel_ = 2;      // 下混声道：0=左(MIC1) 1=右(MIC2) 2=平均
    bool     micDebug_   = false;  // 每 ~0.5s 打 L/R RMS（采集全零排查）
    volatile bool committed_ = false;   // 确认窗是否已过（Recorder commit 置位）
    volatile bool sessionLive_ = false; // 本段会话统计窗口（beginSession 开、stop 清）
    bool     flushTail_ = false;   // 停录后打一行收尾统计的一次性旗子
    bool     cuePending_ = false;  // "嘎"提示待播：第一帧麦克风数据真正到手时才响
    int      micFailCycles_ = 0;   // 录音中 mic 连续读取失败周期（麦克风故障判定）
    bool     micFailReported_ = false;

    // 确认窗帧缓冲（泵任务独占读写；discard 时泵已停读，Recorder 清零无竞争）
    uint8_t  bufPkts_[REC_BUF_PKTS][REC_BUF_PKT_SZ] = {};
    uint16_t bufLen_[REC_BUF_PKTS] = {};
    int      bufCount_ = 0;
    uint32_t framesSent_ = 0;      // 录音统计：成功上行帧数
    uint32_t framesDrop_ = 0;      // 丢弃帧数（未连接/发送失败/缓冲溢出）
    uint32_t recRmsSum_  = 0;      // 能量累计（~0.5s 一行平均 RMS 当"活着"证据）
    uint32_t talkFrames_ = 0;
    uint32_t talkRmsSum_ = 0;
};

}  // namespace gaga
