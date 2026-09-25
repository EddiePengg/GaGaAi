#include "state/Settings.h"
#include "state/MsgLog.h"  // utf8CopyTrunc

#include <cstdio>
#include <cstdlib>

#include "esp_log.h"
#include "nvs_flash.h"

namespace gaga {

static const char* TAG = "gaga.settings";

// NVS 键名（namespace "gaga"）——改键名 = 用户设置清零，慎改
static constexpr const char* kNvs        = "gaga";
static constexpr const char* kKeyVol     = "vol";
static constexpr const char* kKeyBright  = "bright";
static constexpr const char* kKeyScrMs   = "scr_ms";
static constexpr const char* kKeyBeep    = "beep";
static constexpr const char* kKeyKws     = "kws";
static constexpr const char* kKeySubs    = "subs";
static constexpr const char* kKeyShake   = "shake";
static constexpr const char* kKeyLift    = "lift";
static constexpr const char* kKeyARot    = "arot";
static constexpr const char* kKeyFlip    = "scrflip";
static constexpr const char* kKeySsid    = "wifi_ssid";
static constexpr const char* kKeyPass    = "wifi_pass";
static constexpr const char* kKeyWifiOn  = "wifi_on";
static constexpr const char* kKeyTalkPrv = "talk_prv";

void Settings::load() {
    nvs_handle_t h = 0;
    if (nvs_open(kNvs, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "NVS 无设置（首次开机或清空），用默认值");
        return;
    }
    int32_t v = 0;
    if (nvs_get_i32(h, kKeyVol, &v) == ESP_OK) setVolume(static_cast<int>(v));
    if (nvs_get_i32(h, kKeyBright, &v) == ESP_OK) setBrightness(static_cast<int>(v));
    if (nvs_get_i32(h, kKeyScrMs, &v) == ESP_OK) setScreenTimeoutMs(static_cast<uint32_t>(v));
    int8_t b = 0;
    if (nvs_get_i8(h, kKeyBeep, &b) == ESP_OK) beepEnabled_ = (b != 0);
    if (nvs_get_i8(h, kKeyKws, &b) == ESP_OK) kwsEnabled_ = (b != 0);
    if (nvs_get_i8(h, kKeySubs, &b) == ESP_OK) talkSubtitles_ = (b != 0);
    if (nvs_get_i8(h, kKeyShake, &b) == ESP_OK) shakeEnabled_ = (b != 0);
    if (nvs_get_i8(h, kKeyLift, &b) == ESP_OK) liftEnabled_ = (b != 0);
    if (nvs_get_i8(h, kKeyARot, &b) == ESP_OK) autoRotateEnabled_ = (b != 0);
    if (nvs_get_i8(h, kKeyFlip, &b) == ESP_OK) screenFlip_ = (b != 0);
    if (nvs_get_i32(h, kKeyTalkPrv, &v) == ESP_OK) setTalkProviderByIndex(static_cast<int>(v));
    nvs_close(h);
    ESP_LOGI(TAG, "设置已载入：音量 %d 亮度 %d 息屏 %lums 提示音 %s 语音唤醒 %s 字幕 %s 对话引擎 %s",
             volume_, brightness_, static_cast<unsigned long>(screenTimeoutMs_),
             beepEnabled_ ? "开" : "关", kwsEnabled_ ? "开" : "关",
             talkSubtitles_ ? "开" : "关", talkProvider());
}

void Settings::save() {
    nvs_handle_t h = 0;
    if (nvs_open(kNvs, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "NVS 打开失败，设置不落盘");
        return;
    }
    nvs_set_i32(h, kKeyVol, volume_);
    nvs_set_i32(h, kKeyBright, brightness_);
    nvs_set_i32(h, kKeyScrMs, static_cast<int32_t>(screenTimeoutMs_));
    nvs_set_i8(h, kKeyBeep, beepEnabled_ ? 1 : 0);
    nvs_set_i8(h, kKeyKws, kwsEnabled_ ? 1 : 0);
    nvs_set_i8(h, kKeySubs, talkSubtitles_ ? 1 : 0);
    nvs_set_i8(h, kKeyShake, shakeEnabled_ ? 1 : 0);
    nvs_set_i8(h, kKeyLift, liftEnabled_ ? 1 : 0);
    nvs_set_i8(h, kKeyARot, autoRotateEnabled_ ? 1 : 0);
    nvs_set_i8(h, kKeyFlip, screenFlip_ ? 1 : 0);
    nvs_set_i8(h, kKeyWifiOn, wifiEnabled_ ? 1 : 0);
    nvs_set_i32(h, kKeyTalkPrv, talkProviderIdx_);
    const esp_err_t err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) ESP_LOGW(TAG, "NVS commit 失败：%s", esp_err_to_name(err));
}

void Settings::setVolume(int v) {
    if (v < VOL_MIN) v = VOL_MIN;
    if (v > VOL_MAX) v = VOL_MAX;
    volume_ = v;
}

void Settings::setBrightness(int b) {
    if (b < BRIGHT_MIN) b = BRIGHT_MIN;
    if (b > BRIGHT_MAX) b = BRIGHT_MAX;
    brightness_ = b;
}

void Settings::setScreenTimeoutMs(uint32_t ms) {
    // 必须是档位表里的合法值（防 NVS 旧数据/越界写入）
    for (int i = 0; i < SCREEN_TIMEOUT_COUNT; i++) {
        if (SCREEN_TIMEOUTS_MS[i] == ms) {
            screenTimeoutMs_ = ms;
            return;
        }
    }
    screenTimeoutMs_ = 10000;  // 非法值回默认
}

int Settings::timeoutIndex() const {
    for (int i = 0; i < SCREEN_TIMEOUT_COUNT; i++) {
        if (SCREEN_TIMEOUTS_MS[i] == screenTimeoutMs_) return i;
    }
    return 1;  // 默认档 10s
}

void Settings::setWifi(const char* ssid, const char* pass) {
    utf8CopyTrunc(wifiSsid_, sizeof(wifiSsid_), ssid ? ssid : "");
    utf8CopyTrunc(wifiPass_, sizeof(wifiPass_), pass ? pass : "");
}

void Settings::setMqtt(const char* host, int port) {
    utf8CopyTrunc(mqttHost_, sizeof(mqttHost_), host ? host : "");
    mqttPort_ = port;
}

void Settings::setTimeoutByIndex(int idx) {
    if (idx < 0 || idx >= SCREEN_TIMEOUT_COUNT) return;
    screenTimeoutMs_ = SCREEN_TIMEOUTS_MS[idx];
}

void Settings::setTalkProviderByIndex(int idx) {
    if (idx < 0 || idx >= TALK_PROVIDER_COUNT) return;  // 越界忽略（NVS 旧数据）
    talkProviderIdx_ = idx;
}

const char* Settings::talkProvider() const {
    return TALK_PROVIDERS[talkProviderIdx_];
}

}  // namespace gaga
