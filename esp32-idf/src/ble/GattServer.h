#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "net/Link.h"
#include "protocol/frame.h"

// BLE GATT Server（IDF 内置 NimBLE host）。UUID 严格照抄 docs/protocol.md §1。
// 语义与 Arduino 线 esp32/src/ble/GattServer.* 一致：
//   广播名 GAGA-XXXX（BLE MAC 后四位）、MTU 目标 517、断连自动恢复广播、
//   sendFrame 按协商 MTU 分包 notify、RX 写入经 FrameDecoder 重组后回调。
// 发送接口内部自带互斥（多任务并发安全）：app 任务、上行泵、talk 各自直接调。
namespace gaga {

class GattServer : public Link {
public:
    using FrameCallback      = std::function<void(uint8_t type, const uint8_t* payload, uint16_t len)>;
    using ConnectionCallback = std::function<void(bool connected)>;

    // 广播名前缀 "GAGA"；返回是否初始化成功
    bool begin(const char* namePrefix = "GAGA");

    // 连接状态下按当前 MTU 分包发送；未连接返回 false（调用方自行记丢弃日志）
    bool sendFrame(uint8_t type, const uint8_t* payload, uint16_t len) override;
    bool sendJson(const char* json) override;  // JSON 文本包成 type=0x02 帧发给 App

    bool isConnected() const override { return connected_; }
    const char* deviceName() const { return name_; }

    void onFrame(FrameCallback cb)         { frameCb_ = std::move(cb); }
    void onConnection(ConnectionCallback cb) { connCb_ = std::move(cb); }

    // ---- 以下由 NimBLE 回调路径调用（public 仅因 C 回调可达），业务勿调 ----
    void handleRxWrite(const uint8_t* data, size_t len);  // RX 写入 → 喂帧重组器
    void handleConnect(uint16_t connHandle);   // 连接建立：记句柄、回填 TX val_handle
    void handleDisconnect();                   // 断连：清状态、丢残帧、恢复广播
    void handleMtuChange(uint16_t mtu);        // MTU 协商结果落账，决定分包大小
    void startAdvertising();  // 连接失败/断连/sync 时恢复广播

private:

    static constexpr uint16_t DEFAULT_MTU = 23;   // BLE 默认 ATT MTU

    char     name_[16]     = {0};       // 广播名 GAGA-XXXX
    bool     connected_    = false;
    uint16_t connHandle_   = 0xFFFF;    // 当前连接句柄（未连接时无效值）
    uint16_t mtu_          = DEFAULT_MTU;  // 协商后的 MTU，sendFrame 分包依据
    uint16_t txValHandle_  = 0;         // TX 特征值句柄（notify 用；host 启动后才回填）
    SemaphoreHandle_t sendMtx_ = nullptr;  // 发送互斥（app 任务/上行泵/talk 并发）

    FrameDecoder      decoder_;
    FrameCallback     frameCb_;
    ConnectionCallback connCb_;
};

}  // namespace gaga
