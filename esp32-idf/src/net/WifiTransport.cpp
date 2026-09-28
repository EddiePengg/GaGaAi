#include "net/WifiTransport.h"

#include <cstring>
#include <vector>

#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"   // esp_netif_create/destroy_default_wifi_sta
#include "mqtt_client.h"

#include "state/Settings.h"

namespace gaga {

static const char* TAG = "gaga.wifi";

// 上行物理包上限：与手机桥 MTU-3 同量级（一条 MQTT 消息 = 一个物理包，§4）
static constexpr int UPLINK_PACKET_MAX = 500;

static WifiTransport* s_wifi_self = nullptr;  // C 事件回调出口（单实例）

void WifiTransport::mqttEvent(void* handler_args, esp_event_base_t base,
                              int32_t id, void* event_data) {
    auto* self = static_cast<WifiTransport*>(handler_args);
    auto* ev = static_cast<esp_mqtt_event_handle_t>(event_data);
    switch (id) {
    case MQTT_EVENT_CONNECTED:
        self->mqttUp_ = true;
        esp_mqtt_client_subscribe(ev->client, "gaga/down", 1);
        // 重连即清重组状态：上次连接可能死在半帧里，脏状态会吃掉新流的头部
        self->decoder_.reset();
        ESP_LOGI(TAG, "MQTT 已连接，订阅 gaga/down");
        if (self->connCb_) self->connCb_(true);
        break;
    case MQTT_EVENT_DISCONNECTED:
        if (self->mqttUp_) {
            self->mqttUp_ = false;
            ESP_LOGW(TAG, "MQTT 断开（自动重连中）");
            if (self->connCb_) self->connCb_(false);
        }
        break;
    case MQTT_EVENT_DATA:
        // 一条 MQTT 消息 = 一个物理包 → 喂帧重组器（与 BLE 路径完全对称）
        // 下行字节流对账日志（2026-09-28 WiFi 下行错位排查）：len/头 3 字节/累计
        {
            static uint32_t s_rxTotal = 0;
            s_rxTotal += static_cast<uint32_t>(ev->data_len);
            ESP_LOGD("gaga.wifirx", "rx %dB head=%02x %02x %02x total=%lu",
                     static_cast<int>(ev->data_len),
                     ev->data_len > 0 ? static_cast<uint8_t>(ev->data[0]) : 0,
                     ev->data_len > 1 ? static_cast<uint8_t>(ev->data[1]) : 0,
                     ev->data_len > 2 ? static_cast<uint8_t>(ev->data[2]) : 0,
                     static_cast<unsigned long>(s_rxTotal));
        }
        self->decoder_.feed(reinterpret_cast<const uint8_t*>(ev->data),
                            static_cast<size_t>(ev->data_len));
        break;
    default:
        break;
    }
}

void WifiTransport::wifiEvent(void* handler_args, esp_event_base_t base,
                              int32_t id, void* event_data) {
    auto* self = static_cast<WifiTransport*>(handler_args);
    if (id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
        self->onWifiDown();
        esp_wifi_connect();  // 自动重连（断开→重连循环，连上即停）
    } else if (id == IP_EVENT_STA_GOT_IP) {
        self->onWifiUp();
    }
}

void WifiTransport::onWifiUp() {
    wifiUp_ = true;
    ESP_LOGI(TAG, "WiFi 已连（家模式）");
}

void WifiTransport::onWifiDown() {
    if (wifiUp_) ESP_LOGW(TAG, "WiFi 断开，重连中");
    wifiUp_ = false;
}

void WifiTransport::begin(const Settings& settings, bool startRadio) {
    s_wifi_self = this;

    // 网络事件循环 + STA 网卡（重复初始化返回 ERR 视作已就绪）
    if (esp_netif_init() != ESP_OK) { /* 已初始化不算失败 */ }
    if (esp_event_loop_create_default() != ESP_OK) { /* 同上 */ }
    netif_ = esp_netif_create_default_wifi_sta();

    ESP_LOGI(TAG, "[diag] wifi init 前：内部=%u 最大连续=%u",
             static_cast<unsigned>(esp_get_free_internal_heap_size()),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));

    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t e = esp_wifi_init(&wcfg);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init 失败: %s（init 后 内部=%u 最大连续=%u）",
                 esp_err_to_name(e),
                 static_cast<unsigned>(esp_get_free_internal_heap_size()),
                 static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
        return;
    }
    // 第 4 参必须传 this：handler_args 就是回调第 1 参，传 nullptr 则 self 空指针
    // （2026-09-27 实锤：GOT_IP 事件一来自身是 null，self->onWifiUp() 写入 0x70
    // StoreProhibited 崩溃——"拿到 IP 后必重启"的跨版本幽灵真凶）
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                        &wifiEvent, this, &wifiEv_);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                        &wifiEvent, this, &ipEv_);

    if (!startRadio) {
        ESP_LOGI(TAG, "WiFi 预热完成（仅 init：内存已验证+整堆，未起射频）");
        return;
    }

    wifi_config_t sta = {};
    strncpy(reinterpret_cast<char*>(sta.sta.ssid), settings.wifiSsid(),
            sizeof(sta.sta.ssid) - 1);
    strncpy(reinterpret_cast<char*>(sta.sta.password), settings.wifiPass(),
            sizeof(sta.sta.password) - 1);
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &sta);
    esp_wifi_start();
    wifiStarted_ = true;

    if (settings.mqttHost()[0] == '\0') {
        ESP_LOGW(TAG, "broker 未配置，MQTT 跳过（'w' 补 host 字段后重切；30s 后自动回退 BLE）");
        return;
    }

    // MQTT 客户端：broker 地址来自 wifi_cfg 下发（Settings 存储）
    char uri[128];
    snprintf(uri, sizeof(uri), "mqtt://%s:%d", settings.mqttHost(),
             static_cast<int>(settings.mqttPort()));
    esp_mqtt_client_config_t mcfg = {};
    mcfg.broker.address.uri = uri;
    mcfg.session.keepalive = 30;
    mcfg.network.reconnect_timeout_ms = 3000;   // 首连常早于 DHCP 拿 IP，3s 快重试
    mcfg.buffer.size = 2048;      // 下行单包上限（与手机桥帧一致）
    mcfg.buffer.out_size = 1024;
    if (settings.mqttUser()[0] != '\0') {   // broker 开鉴权（ADR-061）时必带
        mcfg.credentials.username = settings.mqttUser();
        mcfg.credentials.authentication.password = settings.mqttPass();
    }
    mqtt_ = esp_mqtt_client_init(&mcfg);
    esp_mqtt_client_register_event(mqtt_, MQTT_EVENT_ANY, mqttEvent, this);
    esp_mqtt_client_start(mqtt_);

    // 下行物理包 → 帧重组 → 业务回调（与 GattServer 路径对称）
    decoder_.onFrame([this](uint8_t type, const uint8_t* payload, uint16_t len) {
        if (frameCb_) frameCb_(type, payload, len);
    });
    decoder_.onError([](const char* reason) {
        ESP_LOGW(TAG, "下行帧重组错误: %s", reason);
    });
    ESP_LOGI(TAG, "WiFi 链路启动（SSID=%s broker=%s）",
             settings.wifiSsid(), settings.mqttHost());
}

// 可逆拆栈：见头文件注释。顺序有讲究——先掐回调再拆驱动，
// 防 STA_DISCONNECTED 里的自动重连打到已 deinit 的句柄上。
void WifiTransport::end() {
    if (mqtt_ != nullptr) {
        esp_mqtt_client_destroy(mqtt_);   // 内含 stop，连不上也不阻塞
        mqtt_ = nullptr;
    }
    // 拆驱动前先掐回调 + 断连停射频，给在途事件留窗口——否则正在连接时拆栈，
    // 连接完成事件会落在已 deinit 的驱动上（use-after-free，真机实锤崩过）
    if (wifiEv_ != nullptr) {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, wifiEv_);
        wifiEv_ = nullptr;
    }
    if (ipEv_ != nullptr) {
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, ipEv_);
        ipEv_ = nullptr;
    }
    if (wifiStarted_) {
        esp_wifi_disconnect();
        esp_wifi_stop();
        vTaskDelay(pdMS_TO_TICKS(150));   // 在途的 connect/IP 事件落完再 deinit
        wifiStarted_ = false;
    }
    esp_wifi_deinit();
    if (netif_ != nullptr) {
        esp_netif_destroy_default_wifi(netif_);
        netif_ = nullptr;
    }
    wifiUp_ = false;
    mqttUp_ = false;
    ESP_LOGI(TAG, "WiFi 栈已拆除（可重开）");
}

// 上行：按物理包切分（≤500B）逐包 publish 到 gaga/up（QoS1 保序完整）
bool WifiTransport::sendFrame(uint8_t type, const uint8_t* payload, uint16_t len) {
    if (!mqttUp_) return false;
    auto packets = encodeFrame(type, payload, len, UPLINK_PACKET_MAX);
    for (auto& pkt : packets) {
        const int rc = esp_mqtt_client_publish(mqtt_, "gaga/up",
                                               reinterpret_cast<const char*>(pkt.data()),
                                               static_cast<int>(pkt.size()), 1, 0);
        if (rc < 0) {
            ESP_LOGW(TAG, "上行 publish 失败 rc=%d", rc);
            return false;
        }
    }
    return true;
}

bool WifiTransport::sendJson(const char* json) {
    return sendFrame(FRAME_TYPE_JSON,
                     reinterpret_cast<const uint8_t*>(json),
                     static_cast<uint16_t>(strlen(json)));
}

}  // namespace gaga
