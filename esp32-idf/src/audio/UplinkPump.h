#pragma once

#include <cstdint>

#include "state/AppState.h"

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
    // 离线缓存上限：≈2.6MB（约 8 分钟语音，50 帧/s × ~130B/帧）
    static constexpr uint32_t CAP_MAX_BYTES = 2600 * 1024;

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
    // ---- 离线缓存（2026-09-25 用户需求：断链别白说）----
    // 录音中每帧同步进 PSRAM（[len16][payload] 序列，段间插 0xFFFF 分界：
    // 多条离线消息各自独立，补发不粘连——2026-09-26 三合一 bug 修复）。
    // 回收规则：rec→Sent（服务端回执）= 送达，清；Sending→Idle（发送失败
    // 或本段见过断链）= 标记待补发；链路恢复按段重放（断点续传，已完成
    // 段不重发）。容量上限 ≈2.6MB（约 8 分钟语音），到顶停止追加但录音照常。
    void watchRecState();
    void capAppend(const uint8_t* pkt, uint16_t n);
    void capMarkSegment();   // 缓存流插 0xFFFF 分界：多条离线消息不粘连
    void capClear();
    void capMarkPending();
    void capReplay();
    bool sendSegStop(uint32_t frames);  // 段收尾 rec_stop（false=链路断）
    void bailReplay();                  // 补发中止：保缓存/续传点，10s 退避

    AppContext* ctx_ = nullptr;

    int      micChanSel_ = 2;      // 下混声道：0=左(MIC1) 1=右(MIC2) 2=平均
    bool     micDebug_   = false;  // 每 ~0.5s 打 L/R RMS（采集全零排查）
    volatile bool committed_ = false;   // 确认窗是否已过（Recorder commit 置位）
    volatile bool sessionLive_ = false; // 本段会话统计窗口（beginSession 开、stop 清）
    volatile bool linkSawDown_ = false; // 本段录音期间手机侧 MQTT 断过（离线补发判定）
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

    // ---- 离线缓存状态 ----
    uint8_t* capBuf_ = nullptr;    // PSRAM 缓存（惰性分配）
    uint32_t capLen_ = 0;          // 已缓存字节数
    uint32_t capCap_ = 0;          // 已分配容量
    uint32_t capFrames_ = 0;       // 已缓存帧数（20ms/帧）
    uint32_t capDurMs_ = 0;        // 待补发段时长
    bool     capFull_ = false;     // 到上限：停止追加，录音照常
    bool     capPending_ = false;  // 有整段录音待补发
    uint32_t capRetryAtMs_ = 0;    // 补发失败的退避时刻（10s 后再试）
    uint32_t capDoneOff_ = 0;      // 补发续传点：最近一个已完成 rec_stop 的段尾
    uint32_t lastStatusQueryMs_ = 0;  // 上次对账查询时刻（节流）
    uint32_t queryIntervalMs_  = 10000; // 对账间隔（追不回时退避，10s→30s）
    uint32_t lastQueryVersion_ = 0;    // 上次查询时的账本版本（没动=没追回→退避）
    int      prevRecState_ = static_cast<int>(RecState::Idle);
    uint32_t recRmsSum_  = 0;      // 能量累计（~0.5s 一行平均 RMS 当"活着"证据）
    uint32_t talkFrames_ = 0;
    uint32_t talkRmsSum_ = 0;
};

}  // namespace gaga
