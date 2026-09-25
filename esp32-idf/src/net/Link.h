#pragma once

#include <cstdint>
#include <functional>

// 链路抽象（ADR-039 B 期）：上行业务代码只认这个接口，不关心底下是
// BLE（GattServer）还是 WiFi+MQTT（WifiTransport）。
// 语义约定：
//   isConnected  = 当前链路健康、可立即发送
//   sendFrame    = 一帧业务数据（内部自动分包，逐包有序送达对端重组）
//   sendJson     = JSON 信令快捷方式（= sendFrame(FRAME_TYPE_JSON, ...)）
namespace gaga {

class Link {
public:
    using FrameCallback = std::function<void(uint8_t type, const uint8_t* payload, uint16_t len)>;
    using ConnectionCallback = std::function<void(bool connected)>;

    virtual ~Link() = default;
    virtual bool isConnected() const = 0;
    virtual bool sendFrame(uint8_t type, const uint8_t* payload, uint16_t len) = 0;
    virtual bool sendJson(const char* json) = 0;

    // 事件注册：下行完整帧 / 链路连断（两种链路行为一致）
    virtual void onFrame(FrameCallback cb) = 0;
    virtual void onConnection(ConnectionCallback cb) = 0;
};

}  // namespace gaga
