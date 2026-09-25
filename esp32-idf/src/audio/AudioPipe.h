#pragma once

#include <cstddef>
#include <cstdint>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "esp_codec_dev.h"
#include "driver/i2s_types.h"

// 音频通路封装（esp_codec_dev 之上，I2S 通道本模块自建——不走 bsp_audio_init）
//   采集：ES7210 双麦 STD 立体声（L=MIC1 / R=MIC2，官方 05_Spec_Analyzer 姿势 A）
//   播放：ES8311 + NS4150B 功放（STD 立体声，PA 脚由 codec 驱动管）
//   提示音：beep(n) 异步合成 1kHz 正弦（参照 Arduino 线 ADR-018；talk 期间禁用）
//
// 布线依据（全部来自真机/官方源码实证，勿猜；TDM 4 槽弯路见 bug-analysis-0923.md）：
// ② 双工同速率约束（esp_codec_dev check_fs_compatible）：录/放同时打开时采样率必须一致
//   → M1 录音态双通道 16kHz（官方 05 原生姿势）；M5 talk 态双通道 24kHz
//   （下行 ogg_opus 原生 24k，上行 24k→16k 抽取；出厂语音通路也是 24kHz）。
// ③ I2S 槽位/模式初始化后不再变（TX/RX 恒 STD 立体声），open 只做时钟重配——
//   slot 重配会重分配 DMA，内存紧张时 esp_codec_dev 善后路径会 NULL 解引用（真机踩实）。
// ④ STD 双工 RX 跟随 TX 时钟：扬声器一关（TX disable）麦克风断粮——
//   spkClose 在 mic 开着时延期（登记 spkWantClose_，micClose 时才落地）。
// ⑤ ES8311 ASDOUT 与 ES7210 SDOUT1 共用 GPIO10（原理图 net I2S_ASDOUT）：
//   es8311_open 无条件 REG0E=0x02 把 ADC 上电，ASDOUT 驱动全零盖住 ES7210 数据——
//   每次 spkOpen 后必须把 REG0E 写回 0x00（ADC/PGA 下电，ASDOUT 高阻；Arduino 线实锤）。
namespace gaga {

class AudioPipe {
public:
    // 采样率双档（duplex 同速率约束②不违反——同一相位内录/放同速率）：
    //   M1 录音态：16kHz（官方 05_Spec_Analyzer 原生姿势；上行 Opus 原生 16k 零重采样）
    //   M5 talk 态：24kHz（下行 ogg_opus 原生 24k；上行 24k→16k 抽取）
    static constexpr int RATE_REC  = 16000;  // M1 录音/提示音
    static constexpr int RATE_TALK = 24000;  // M5 全双工
    static constexpr int MIC_CHANNELS = 2;   // ES7210 双麦立体声（L=MIC1/R=MIC2）

    bool begin();  // I2S（收发都是 STD 立体声）+ ES8311 + ES7210 初始化（幂等）

    // ---- 采集（STD 立体声：L=MIC1 / R=MIC2，一次读入交织样本）----
    bool micOpen(int rate);   // 打开 ES7210 采集；rate 必须与播放同档（约束②）
    void micClose();          // 关采集；此前延期的 spkClose 在此落地（约束④）
    bool micOpened() const { return micOpened_; }
    int  micRate() const { return micRate_; }
    // 读 PCM（16bit 立体声交织，L=MIC1/R=MIC2），阻塞至凑满或超时；返回实际字节数（<0 失败）。
    // 直接读自建 I2S 通道（绕过 esp_codec_dev 的 data_if：它会把字节数吞掉，
    // 且 in_reconfig 卡死时会 memset 全零——采集全零排查后选择直连）
    int  micRead(uint8_t* buf, int maxBytes);

    // ---- 播放 ----
    bool spkOpen(int rate);   // 开 ES8311+PA；open 后必须立刻 REG0E 下电让线（约束⑤）
    void spkClose();          // 关播放、断 PA；mic 还开着就只登记延期（约束④）
    // 双工同开关（官方 bsp_extra_codec_set_fs 姿势）：换档必须 mic+spk 一起关再一起开。
    // 按需上电模型（ADR-027，推翻 ADR-023 的常开）：会话开始才上电、结束整条断电——
    // 录音/talk 开始时上电，结束时断电回零耗；
    // 禁止在提示音/下行播放中途调用（重配会掐断在播音频）。
    bool duplexOpen(int rate);
    bool spkOpened() const { return spkOpened_; }
    int  spkRate() const { return spkRate_; }
    // 写立体声 PCM（16bit 交织），返回已写字节数
    int  spkWrite(const uint8_t* buf, int bytes);
    // 写单声道 PCM（内部 L=R 复制成立体声；槽位恒立体声的约束见文件头注释③）
    int  spkWriteMono(const int16_t* mono, int samples);
    // ES8311 REG0E=0x00：ADC/PGA 下电，ASDOUT 高阻让线给 ES7210（约束⑤）
    void spkAdcPowerDown();

    // 串口 'e'：寄存器回读自检（采集全零排查用；codec 需处于 open 状态）
    void dumpCodecRegs();

    // 串口 'B'：直出 1kHz 长音（默认 1000ms，测量用；GPIO 探针在这期间抓 DOUT 翻转）
    int  playToneDebug(int ms);

    // ---- 用户设置（Settings 注入，ADR-031）----
    // 播放音量 0~100：存值 + 播放开着就立即下发 ES8311 out vol；
    // 之后每次 spkOpen 都会重设（open 序列会把 codec 恢复默认音量）
    void setVolume(int pct);
    // 提示音总开关（beep/chime 入口空转；talk 下行语音不受影响）
    void setBeepEnabled(bool on) { beepEnabled_ = on; }

    // ---- 音频作业队列（串行执行，ADR-027）：上电/断电/播放 绝不并发——
    // 2026-09-23 真机实锤两类事故都源于并发/次序错误：开关 codec 的 I2S 舞蹈
    // 掐碎在播的"滴"；spk 单独上电的冷路径不出数（DOUT8 零翻转）。现在一切走
    // FIFO 作业：上电完成 → 才播 → 才断电，两个坑被顺序根治。
    void beep(int count);                    // 兼容保留（开机自检等旧路径）
    void quack(int count);                   // 鸭叫"嘎"×count（采样版）
    void poot();                             // "噗噗"鸭子放屁（错误专属音效）
    // ---- 音效（2026-09-25 定稿，无方案选择）：嘎=录开/发，叮咚=收，噗噗=错 ----
    enum class SndEv : uint8_t { Listen, Sent, Received, Error };
    void playQuackSample(int count, bool closeAfter = false);  // 播采样×count（closeAfter=本作业自开的通路才收）
    // 统一事件出口：调用方只说"发生了什么"，音效在此分发
    void event(SndEv ev);
    void chime();                            // "叮咚"双音（已由嘎嘎嘎替代，保留待清）
    void requestOpen(int rate = RATE_REC);   // 入队上电（会话开始；RATE_REC/RATE_TALK）
    void requestClose();                     // 入队断电（会话结束，整条音频前端回零耗）
    // 队列是否还有作业在飞（上电/播放/断电）——录音确认窗/换档等它落地时用
    bool beepBusy() const { return beepBusy_; }
    // talk 开始/结束时调用：talk 期间 beep 变空转日志
    void setTalkActive(bool active) { talkActive_ = active; }

    // ---- 工具 ----
    // 立体声下混为单声道：sel=0 左(MIC1)/1 右(MIC2)/2 平均（串口 'm' 切换）
    static void downmix(const int16_t* stereo, int16_t* mono, int frames, int sel);
    // 24kHz → 16kHz（2:3）重采样：3 抽头预滤波 + 线性插值（语音 ASR 足够）
    static void resample24to16(const int16_t* in480, int16_t* out320);
    // RMS（采集存活/能量日志用）
    static uint32_t rms(const int16_t* pcm, int samples);

private:
    // I2S/codec 数据通路互斥（真机实锤 2026-09-25：esp_codec_dev 非线程安全，
    // audioTask 播放/uplink micRead/串口 'B' 直写并发 → FreeRTOS 链表损坏 boot
    // loop；作业队列只串行化"作业"，数据读写也要串行）
    SemaphoreHandle_t pipeMtx_ = nullptr;
    void pipeLock()   { if (pipeMtx_) xSemaphoreTakeRecursive(pipeMtx_, portMAX_DELAY); }
    void pipeUnlock() { if (pipeMtx_) xSemaphoreGiveRecursive(pipeMtx_); }

    // 音频作业（FIFO 串行执行，ADR-027）：op 0=播放(arg=声数) 1=上电(arg=采样率/1000)
    //   2=断电  3="叮咚"双音(arg 忽略)——上电/播放/断电绝不并发，只在作业线程里排队做
    struct Job { uint8_t op; uint16_t arg; };
    static void audioTask(void*);  // 作业线程：唯一动音频前端的地方，先后次序在此保证
    void enqueue(const Job& j);    // 投递作业；队列满直接丢弃（宁缺勿堵）

    esp_codec_dev_handle_t spk_ = nullptr;
    esp_codec_dev_handle_t mic_ = nullptr;
    const audio_codec_data_if_t* dataIf_ = nullptr;  // codec 数据接口（播放写仍走它）
    i2s_chan_handle_t rxChan_ = nullptr;             // 自建 RX TDM 通道（micRead 直连）
    i2s_chan_handle_t txChan_ = nullptr;             // 自建 TX STD 通道
    bool micOpened_ = false;
    bool spkOpened_ = false;
    bool spkWantClose_ = false;   // mic 开着时 spkClose 延期登记（约束④）
    int  micRate_   = 0;
    int  spkRate_   = 0;
    bool talkActive_ = false;
    int  volume_     = 85;      // ES8311 out vol 百分比（原 SPK_VOLUME 常量，ADR-031 转用户设置）
    bool beepEnabled_ = true;   // 提示音总开关

    QueueHandle_t  beepQueue_ = nullptr;   // 元素：Job（音频作业 FIFO）
    TaskHandle_t   beepTask_  = nullptr;
    volatile bool  beepBusy_  = false;     // 队列有货或作业执行中
};

}  // namespace gaga
