#pragma once

#include <cstdint>

// 设备端用户设置（ADR-031"设备即应用"路线的第一步：设置做在设备触摸屏上，不进 App）
//   音量     0~100   → ES8311 out vol（esp_codec_dev_set_out_vol，spkOpen 后立即生效）
//   亮度     10~100  → CO5300 0x51 寄存器（bsp_display_brightness_set）
//   息屏时长 5/10/30/60s → AppState 自动息屏计时
//   提示音   开/关   → AudioPipe beep/chime 空转
// 存储：NVS namespace "gaga"（低频写：每次拖完 slider 才落一次，寿命无忧）。
// 所有字段改动即刻 save()——设置页的交互语义就是"所见即所存"。
namespace gaga {

class Settings {
public:
    static constexpr int VOL_MIN = 0;
    static constexpr int VOL_MAX = 100;
    static constexpr int BRIGHT_MIN = 10;   // AMOLED 低于 10% 近乎全黑，没意义
    static constexpr int BRIGHT_MAX = 100;

    // 息屏时长档位（设置页 < > 循环切换）
    static constexpr uint32_t SCREEN_TIMEOUTS_MS[] = {5000, 10000, 30000, 60000};
    static constexpr int SCREEN_TIMEOUT_COUNT = 4;

    // 对话引擎档位（ADR-035）：talk 用哪个实时后端——选择权在小设备（用户拍板），
    // talk_request.provider 下发，服务端据此建连；标签给设置页循环按钮显示。
    static constexpr const char* TALK_PROVIDERS[]       = {"volc", "step"};
    static constexpr const char* TALK_PROVIDER_LABELS[] = {"豆包", "星辰"};
    static constexpr int TALK_PROVIDER_COUNT = 2;

    void load();   // 上电读 NVS（键缺失/值非法一律回默认，不信任旧分区残留）
    void save();   // 全量写回 + commit

    int volume() const { return volume_; }
    void setVolume(int v);            // clamp 0~100
    int brightness() const { return brightness_; }
    void setBrightness(int b);        // clamp 10~100
    uint32_t screenTimeoutMs() const { return screenTimeoutMs_; }
    void setScreenTimeoutMs(uint32_t ms);   // 非法值回 10s 默认
    bool beepEnabled() const { return beepEnabled_; }
    void setBeepEnabled(bool on) { beepEnabled_ = on; }
    // 语音唤醒（"Hi,乐鑫"，mic 常开本地推理）：默认关——用户拍板不想要
    // 麦克风一直开着；开=KeywordWake 拉起（ADR-037）
    bool kwsEnabled() const { return kwsEnabled_; }
    void setKwsEnabled(bool on) { kwsEnabled_ = on; }
    // 对话字幕（talk 期间 ASR/回复文字上屏）：长文本重排会让低端场景掉帧，
    // 关=纯语音模式（ADR-034 用户反馈"长按一卡一卡"）
    bool talkSubtitles() const { return talkSubtitles_; }
    void setTalkSubtitles(bool on) { talkSubtitles_ = on; }
    // 摇动录音（"摇奶茶"触发，ADR-040）：默认开；摔倒/磕碰会误触，
    // 用户要求给手动开关（2026-09-25）
    bool shakeEnabled() const { return shakeEnabled_; }
    void setShakeEnabled(bool on) { shakeEnabled_ = on; }
    // 抬手亮屏（挂脖拎起自动点亮）：默认开；用户手动开关（2026-09-25）
    bool liftEnabled() const { return liftEnabled_; }
    void setLiftEnabled(bool on) { liftEnabled_ = on; }
    // 自动转向（持握角变化内容跟着转 90° 档）：默认开（2026-09-25）
    bool autoRotateEnabled() const { return autoRotateEnabled_; }
    void setAutoRotateEnabled(bool on) { autoRotateEnabled_ = on; }
    // 音效已定稿无方案选择（2026-09-25）：嘎=录开/发，叮咚=收，噗噗=错
    // ---- 家模式 WiFi（ADR-039）：启用后重启进 WiFi 直连 MQTT（BLE 栈不启动）----
    bool     wifiEnabled() const { return wifiEnabled_; }
    void     setWifiEnabled(bool on) { wifiEnabled_ = on; }
    // ---- 家模式 WiFi 凭证（App 经 BLE 下发存 NVS）----
    bool     wifiConfigured() const { return wifiSsid_[0] != '\0'; }
    const char* wifiSsid() const { return wifiSsid_; }
    const char* wifiPass() const { return wifiPass_; }
    void setWifi(const char* ssid, const char* pass);
    void setMqtt(const char* host, int port);
    const char* mqttHost() const { return mqttHost_; }
    int mqttPort() const { return mqttPort_; }
    // 对话引擎（ADR-035）：档位下标 + 信令用的 provider 值
    int talkProviderIndex() const { return talkProviderIdx_; }
    void setTalkProviderByIndex(int idx);   // 越界忽略
    const char* talkProvider() const;

    // 档位工具：值 ↔ 下标（设置页循环按钮用）；值不在档位表里返回最近档的下一个
    int timeoutIndex() const;
    void setTimeoutByIndex(int idx);

private:
    int      volume_          = 85;
    int      brightness_      = 100;
    uint32_t screenTimeoutMs_ = 10000;
    bool     beepEnabled_     = true;
    bool     kwsEnabled_      = false;
    bool     talkSubtitles_   = true;
    bool     shakeEnabled_    = true;
    bool     liftEnabled_     = true;
    bool     autoRotateEnabled_ = true;
    int      talkProviderIdx_ = 0;  // 默认 volc（豆包）
    char     wifiSsid_[33] = {};
    char     wifiPass_[64] = {};
    char     mqttHost_[64] = {};
    int      mqttPort_ = 1883;
    bool     wifiEnabled_ = false;
};

}  // namespace gaga
