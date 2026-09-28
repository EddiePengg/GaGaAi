#include "net/LinkManager.h"

#include "esp_log.h"

#include "compat.h"
#include "state/Settings.h"

namespace gaga {

static const char* TAG = "gaga.net";

void LinkManager::begin(const Settings& settings) {
    settings_ = &settings;

    // 内部接线：两条链路的回调都先过"是否活动"闸门，再转发业务回调
    ble_.onFrame([this](uint8_t type, const uint8_t* payload, uint16_t len) {
        if (active_ == Active::Ble && frameCb_) frameCb_(type, payload, len);
    });
    ble_.onConnection([this](bool up) {
        if (active_ != Active::Ble) return;
        bleDownAt_ = up ? 0 : millis();
        if (connCb_) connCb_(up);
    });
    wifi_.onFrame([this](uint8_t type, const uint8_t* payload, uint16_t len) {
        if (active_ == Active::Wifi && frameCb_) frameCb_(type, payload, len);
    });
    wifi_.onConnection([this](bool up) {
        if (active_ != Active::Wifi) return;
        if (up) {
            wifiDownAt_ = 0;
            wifiStartAt_ = 0;   // 起链成功，清"起不来"判据
        } else {
            wifiDownAt_ = millis();
        }
        if (connCb_) connCb_(up);
    });

    // WiFi 预热已退役（2026-09-27 深夜）：它本是排障期验证"WiFi 能否 init"的脚手架，
    // 诊断使命完成后只剩负债——WiFi deinit 有已知泄漏，每次开机跑一遍基线掉一截。
    // 开机回归干净语义：只启 BLE；WiFi 的唯一入口是切换那一刻（自动故障转移/'N'）。

    // 开机默认 BLE 外出模式；家模式靠故障转移或手动切过去。
    // 模式偏好从 NVS 恢复（设置页/串口 'N' 共用）：
    //   强制WiFi 但无凭证 = 配置缺失，回退 BLE 并告警（不傻等 30s 超时）
    switch (settings_->linkMode()) {
    case 1:  mode_ = Mode::ForceBle;  break;
    case 2:  mode_ = Mode::ForceWifi; break;
    default: mode_ = Mode::Auto;      break;
    }
    if (mode_ == Mode::ForceWifi && !settings_->wifiConfigured()) {
        ESP_LOGW(TAG, "[net] 偏好在家·WiFi 但无凭证，回退外出·BLE（'w'/APK 配网后生效）");
        mode_ = Mode::Auto;
    }
    if (mode_ == Mode::ForceWifi) {
        // 不在 boot 里直接起 WiFi：boot 堆碎片化会让 WiFi init 多付 ~8KB 税
        // （实锤：直连开机后只剩 1.7KB，刷屏 3.7KB 分不出=残影复发）。
        // 先正常启 BLE，3 秒后走成熟的切换路径（拆 BLE 腾出大连续块再 init）。
        startBle();
        wifiDeferAt_ = millis() + 3000;
        ESP_LOGI(TAG, "[net] 偏好在家·WiFi：先启 BLE，3s 后切换");
    } else {
        startBle();
    }
    ESP_LOGI(TAG, "[net] 模式=%s 活动=%s（自动切换=%s）",
             modeName(), activeName(), settings_->wifiConfigured() ? "开" : "关");
}

const char* LinkManager::modeName() const {
    switch (mode_) {
    case Mode::Auto:      return "Auto";
    case Mode::ForceBle:  return "强制BLE";
    case Mode::ForceWifi: return "强制WiFi";
    }
    return "?";
}

const char* LinkManager::activeName() const {
    switch (active_) {
    case Active::Ble:  return "BLE";
    case Active::Wifi: return "WiFi";
    case Active::None: return "无";
    }
    return "?";
}

void LinkManager::setMode(Mode m) {
    mode_ = m;
    // 经回调落盘（Settings NVS），设置页/串口 'N'/重启恢复 三处同源
    if (modePersistCb_) modePersistCb_(static_cast<int>(m));
    ESP_LOGI(TAG, "[net] 模式 → %s", modeName());
    // 强制模式立即生效；Auto 交给 tick 自然演化（下次评估即修正）
    if (m == Mode::ForceBle && active_ != Active::Ble) switchTo(Active::Ble);
    if (m == Mode::ForceWifi && active_ != Active::Wifi) switchTo(Active::Wifi);
}

void LinkManager::startBle() {
    active_ = Active::Ble;
    bleDownAt_ = 0;
    ble_.begin();
}

void LinkManager::startWifi() {
    active_ = Active::Wifi;
    wifiStartAt_ = millis();
    wifiDownAt_ = 0;
    wifi_.begin(*settings_);
}

void LinkManager::stopActive() {
    if (active_ == Active::Ble) {
        ble_.end();       // 可逆拆栈（nimble_port_stop→deinit，控制器随之下电）
    } else if (active_ == Active::Wifi) {
        wifi_.end();      // 可逆拆栈（mqtt destroy→wifi stop/deinit）
    }
    active_ = Active::None;
}

void LinkManager::switchTo(Active target) {
    if (target == active_) return;
    ESP_LOGI(TAG, "[net] 链路切换 %s → %s", activeName(),
             target == Active::Ble ? "BLE" : "WiFi");
    // 先通知业务侧"链路断了"（talk 收尾/状态栏灭点），再拆旧起新
    if (connCb_ && isConnected()) connCb_(false);
    stopActive();
    if (target == Active::Ble) startBle();
    else                       startWifi();
}

void LinkManager::tick() {
    const uint32_t now = millis();

    // 开机延迟切换：偏好在家·WiFi 时，等 boot 堆稳定后走切换路径（见 begin）
    if (wifiDeferAt_ != 0 && now > wifiDeferAt_) {
        wifiDeferAt_ = 0;
        if (mode_ == Mode::ForceWifi && active_ == Active::Ble) switchTo(Active::Wifi);
    }

    // BLE 广播看门狗（仅 BLE 活动期；家模式 BLE 栈整个不在，省调用）
    if (active_ == Active::Ble) ble_.tick();

    if (mode_ == Mode::Auto) {
        // 故障转移①：BLE 死了 grace 未复连 → 有 WiFi 凭证就切 WiFi
        // （凭证在 = 用户配过家模式，即意图本身；wifiEnabled 开关位退役）
        if (active_ == Active::Ble && bleDownAt_ != 0 &&
            now - bleDownAt_ > BLE_GRACE_MS &&
            settings_->wifiConfigured()) {
            ESP_LOGW(TAG, "[net] BLE %lus 未复连，切换家模式（WiFi）",
                     (unsigned long)(now - bleDownAt_) / 1000);
            switchTo(Active::Wifi);
        }
        // 故障转移②：WiFi 起不来 / 掉了太久 → 回 BLE
        if (active_ == Active::Wifi) {
            if (wifiStartAt_ != 0 && now - wifiStartAt_ > WIFI_FAIL_MS) {
                ESP_LOGW(TAG, "[net] WiFi %lus 未就绪，回退 BLE 外出模式",
                         (unsigned long)WIFI_FAIL_MS / 1000);
                switchTo(Active::Ble);
            } else if (wifiDownAt_ != 0 && now - wifiDownAt_ > WIFI_DOWN_MS) {
                ESP_LOGW(TAG, "[net] WiFi 掉线超 %lus，回退 BLE 外出模式",
                         (unsigned long)WIFI_DOWN_MS / 1000);
                switchTo(Active::Ble);
            }
        }
    }
}

bool LinkManager::isConnected() const {
    if (active_ == Active::Ble)  return ble_.isConnected();
    if (active_ == Active::Wifi) return wifi_.isConnected();
    return false;
}

bool LinkManager::sendFrame(uint8_t type, const uint8_t* payload, uint16_t len) {
    if (active_ == Active::Ble)  return ble_.sendFrame(type, payload, len);
    if (active_ == Active::Wifi) return wifi_.sendFrame(type, payload, len);
    return false;
}

bool LinkManager::sendJson(const char* json) {
    if (active_ == Active::Ble)  return ble_.sendJson(json);
    if (active_ == Active::Wifi) return wifi_.sendJson(json);
    return false;
}

}  // namespace gaga
