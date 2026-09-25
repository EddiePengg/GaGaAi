#pragma once

// 装配上下文：模块指针集合。main.cpp 装配时填一份，
// Recorder / SerialCmd / UplinkPump 经它拿兄弟模块——免去层层 setter，
// 也让"谁用谁"一眼可读（重构 ADR-038）。
namespace gaga {

class AppState;
class MsgLog;
class Settings;
class Ui;
class AudioPipe;
class Link;
class TalkSession;
class OpusEnc;
class Recorder;
class UplinkPump;
class Wake;
class Power;
class Qmi8658;
class KeywordWake;

struct AppContext {
    AppState*    app     = nullptr;  // 状态机（屏幕/录音/talk）
    MsgLog*      log     = nullptr;  // 消息卡数据
    Settings*    settings = nullptr; // 用户设置（NVS）
    Ui*          ui      = nullptr;  // 界面
    AudioPipe*   audio   = nullptr;  // 音频前端
    Link*        link    = nullptr;  // 当前链路（BLE 或 WiFi，ADR-039）
    TalkSession* talk    = nullptr;  // realtime 对话
    OpusEnc*     enc     = nullptr;  // Opus 编码器
    Recorder*    rec     = nullptr;  // 按住说话（确认窗/宽限/建卡）
    UplinkPump*  pump    = nullptr;  // 上行泵（mic→Opus→BLE）
    Wake*        wake    = nullptr;  // 唤醒检测（tap/抬手）
    Power*       power   = nullptr;  // 低功耗
    KeywordWake* kws     = nullptr;  // 语音唤醒（"Hi,乐鑫"，默认关）
};

}  // namespace gaga
