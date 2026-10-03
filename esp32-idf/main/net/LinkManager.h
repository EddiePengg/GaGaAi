#pragma once

#include <cstdint>

#include "net/Link.h"
#include "ble/GattServer.h"
#include "net/WifiTransport.h"

// 链路总机（2026-09-27 家模式互斥 + 自动故障转移，ADR-039/新 ADR）：
// 业务侧（pump/talk/UI）只面对这一个 Link 实现，它把调用转发给当前
// 活动链路（BLE 外出 / WiFi+MQTT 家模式），并持有自动切换状态机：
//   BLE 断开 grace 60s 未复连 且 有 WiFi 凭证 → 拆 BLE 起 WiFi
//   WiFi 30s 起不来 / 连上后掉线 90s        → 拆 WiFi 回 BLE
// 滞回参数防边界抖动；拆栈走可逆路径（deinit 不 mem_release，随时能开回）。
// 手动模式（串口 'N'）强制一边，自动策略暂停。
// settings.wifiEnabled() = 允许自动切 WiFi 的总开关（凭证在 wifi_cfg 下发存 NVS）。
namespace gaga {

class Settings;

class LinkManager : public Link {
public:
    enum class Mode { Auto, ForceBle, ForceWifi };   // Auto = 故障转移

    void begin(const Settings& settings);
    void tick();                    // appTask 10ms 节拍：状态机 + BLE 广播看门狗
    void setMode(Mode m);           // 手动覆盖（设置页/串口 'N' 共用；经 onModePersist 落盘）
    Mode mode() const { return mode_; }
    const char* modeName() const;

    // 模式变更落盘回调（main 接到 Settings NVS；LinkManager 不直接写 Settings）
    void onModePersist(std::function<void(int)> cb) { modePersistCb_ = std::move(cb); }

    // ---- Link 接口（对业务透明，转发给活动链路）----
    bool isConnected() const override;
    bool sendFrame(uint8_t type, const uint8_t* payload, uint16_t len) override;
    bool sendJson(const char* json) override;
    void onFrame(FrameCallback cb) override { frameCb_ = std::move(cb); }
    void onConnection(ConnectionCallback cb) override { connCb_ = std::move(cb); }

    // 当前活动链路（调试日志用）
    const char* activeName() const;

private:
    enum class Active { None, Ble, Wifi };

    static constexpr uint32_t BLE_GRACE_MS  = 60000;  // BLE 断→探 WiFi 的等待
    static constexpr uint32_t WIFI_FAIL_MS  = 30000;  // WiFi 起链超时 = 失败
    static constexpr uint32_t WIFI_DOWN_MS  = 90000;  // WiFi 掉线宽限 = 放弃

    void startBle();
    void startWifi();
    void stopActive();
    void switchTo(Active target);

    GattServer     ble_;
    WifiTransport  wifi_;
    const Settings* settings_ = nullptr;
    Active  active_    = Active::None;
    Mode    mode_      = Mode::Auto;
    uint32_t bleDownAt_   = 0;   // BLE 断连时刻（0=连着）
    uint32_t wifiStartAt_ = 0;   // WiFi 起链时刻（判起不来用）
    uint32_t wifiDownAt_  = 0;   // WiFi 掉线时刻（0=连着）
    uint32_t wifiDeferAt_ = 0;   // 开机偏好在家·WiFi 的延迟切换时刻（0=无）

    FrameCallback      frameCb_;
    ConnectionCallback connCb_;
    std::function<void(int)> modePersistCb_;
};

}  // namespace gaga
