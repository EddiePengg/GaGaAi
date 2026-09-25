#pragma once

#include <cstdint>
#include <functional>

#include "esp_event_base.h"
#include "mqtt_client.h"

#include "net/Link.h"
#include "protocol/frame.h"

// 家模式链路（ADR-039 B 期）：设备直连 WiFi → MQTT，与手机桥完全等价。
// 协议镜像（docs/protocol.md §4）：一条 MQTT 消息 = 一个物理包；
//   上行 gaga/up 逐包 publish（qos1 保序完整）
//   下行 gaga/down 订阅（qos1），逐包喂 FrameDecoder 重组出完整帧
// 家模式下 BLE 栈整体不启动——两种协议栈互斥，片上内存各自宽裕（ADR-039）。
namespace gaga {

class Settings;

class WifiTransport : public Link {
public:
    using FrameCallback = std::function<void(uint8_t type, const uint8_t* payload, uint16_t len)>;
    using ConnectionCallback = std::function<void(bool connected)>;

    // 读 Settings 的 WiFi 凭证与 broker 参数，起 WiFi(STA)+MQTT（异步，事件驱动）
    void begin(const Settings& settings);

    bool isConnected() const override { return wifiUp_ && mqttUp_; }
    bool sendFrame(uint8_t type, const uint8_t* payload, uint16_t len) override;
    bool sendJson(const char* json) override;

    void onFrame(FrameCallback cb) override { frameCb_ = cb; }
    void onConnection(ConnectionCallback cb) override { connCb_ = cb; }

private:
    static void mqttEvent(void* handler_args, esp_event_base_t base,
                          int32_t id, void* event_data);
    static void wifiEvent(void* handler_args, esp_event_base_t base,
                          int32_t id, void* event_data);
    void onWifiUp();
    void onWifiDown();

    FrameCallback frameCb_;
    ConnectionCallback connCb_;
    esp_mqtt_client_handle_t mqtt_ = nullptr;
    FrameDecoder decoder_;                // 下行帧重组（与 GattServer 路径对称）
    bool wifiUp_ = false;
    bool mqttUp_ = false;
};

}  // namespace gaga
