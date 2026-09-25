#include "GattServer.h"

#include <cstdio>
#include <cstring>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_mac.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char* TAG = "gaga.ble";

// UUID 严格照抄 docs/protocol.md §1，禁止改动。
// 128-bit UUID 按 NimBLE 约定以"字符串显示序的反序"填入。
static const ble_uuid128_t kSvcUuid =  // 6e400001-b5a3-f393-e0a9-e50e24dcca9e
    BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
                     0x93, 0xf3, 0xa3, 0xb5, 0x01, 0x00, 0x40, 0x6e);
static const ble_uuid128_t kRxUuid =  // 6e400002 App → 设备（Write）
    BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
                     0x93, 0xf3, 0xa3, 0xb5, 0x02, 0x00, 0x40, 0x6e);
static const ble_uuid128_t kTxUuid =  // 6e400003 设备 → App（Notify）
    BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
                     0x93, 0xf3, 0xa3, 0xb5, 0x03, 0x00, 0x40, 0x6e);

namespace gaga {

static GattServer* s_inst = nullptr;
static uint8_t     s_addrType = 0;

// ---- GATT 访问回调（NimBLE host 任务上下文）----
// RX 特征被 App 写入：把 mbuf 拉平成连续缓冲后交给帧重组器
static int rxAccess(uint16_t connHandle, uint16_t attrHandle,
                    ble_gatt_access_ctxt* ctxt, void* arg) {
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR && s_inst) {
        // 把 mbuf 链拉平再喂重组器（BLE 写包通常单 mbuf，拉平成本可忽略）
        const uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
        uint8_t buf[544];  // 单包物理上限 = MTU(517) + ATT 头，留一档余量
        if (len <= sizeof(buf)) {
            if (ble_hs_mbuf_to_flat(ctxt->om, buf, len, nullptr) == 0) {
                s_inst->handleRxWrite(buf, len);
            }
        } else {
            ESP_LOGW(TAG, "rx write 超长 %u，丢弃", len);
        }
    }
    return 0;
}

static int txAccess(uint16_t, uint16_t, ble_gatt_access_ctxt*, void*) {
    return BLE_ATT_ERR_REQ_NOT_SUPPORTED;  // TX 仅 Notify，不支持读
}

// GATT 表（静态存储；TX 的 val_handle 指向 s_txValHandle，begin 后可读回）
static uint16_t s_txValHandle = 0;
static ble_gatt_chr_def g_chrs[] = {
    {   // TX：设备 → App，Notify
        .uuid       = &kTxUuid.u,
        .access_cb  = txAccess,
        .flags      = BLE_GATT_CHR_F_NOTIFY,
        .val_handle = &s_txValHandle,
    },
    {   // RX：App → 设备，Write / Write No Response
        .uuid      = &kRxUuid.u,
        .access_cb = rxAccess,
        .flags     = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
    },
    { 0 },
};
static ble_gatt_svc_def g_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &kSvcUuid.u,
        .characteristics = g_chrs,
    },
    { 0 },
};

// ---- GAP 事件 ----
static int gapEvent(ble_gap_event* event, void* arg) {
    if (!s_inst) return 0;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_inst->handleConnect(event->connect.conn_handle);
        } else {
            ESP_LOGW(TAG, "connect failed status=%d", event->connect.status);
            s_inst->startAdvertising();
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "disconnected reason=0x%x", event->disconnect.reason);
        s_inst->handleDisconnect();
        break;
    case BLE_GAP_EVENT_MTU:
        s_inst->handleMtuChange(event->mtu.value);
        break;
    case BLE_GAP_EVENT_SUBSCRIBE:
        ESP_LOGI(TAG, "subscribe: conn=%u attr=%u notify=%d",
                 event->subscribe.conn_handle, event->subscribe.attr_handle,
                 event->subscribe.cur_notify);
        break;
    default:
        break;
    }
    return 0;
}

// 组装广播参数并开广播：广播包放 Service UUID，名字放扫描响应包
void GattServer::startAdvertising() {
    // 广播包：flags + 完整 128-bit Service UUID（App 按名称前缀 + UUID 过滤）
    ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof(fields));
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128 = &kSvcUuid;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;
    if (ble_gap_adv_set_fields(&fields) != 0) {
        ESP_LOGE(TAG, "adv set fields 失败");
        return;
    }

    // 设备名放扫描响应包（广播包 31B 放不下 UUID+名字）
    ble_hs_adv_fields rsp;
    memset(&rsp, 0, sizeof(rsp));
    rsp.name = reinterpret_cast<uint8_t*>(name_);
    rsp.name_len = static_cast<uint8_t>(strlen(name_));
    rsp.name_is_complete = 1;
    if (ble_gap_adv_rsp_set_fields(&rsp) != 0) {
        ESP_LOGE(TAG, "scan rsp set fields 失败");
        return;
    }

    ble_gap_adv_params params;
    memset(&params, 0, sizeof(params));
    params.conn_mode = BLE_GAP_CONN_MODE_UND;   // 可连接
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;   // 通用可发现
    const int rc = ble_gap_adv_start(s_addrType, NULL, BLE_HS_FOREVER,
                                     &params, gapEvent, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv start 失败 rc=%d", rc);
        return;
    }
    ESP_LOGI(TAG, "advertising as %s", name_);
}

// host 就绪：确保有地址 → 设设备名 → 开广播
static void onSync() {
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &s_addrType);
    uint8_t addr[6] = {0};
    ble_hs_id_copy_addr(s_addrType, addr, NULL);
    ESP_LOGI(TAG, "addr %02X:%02X:%02X:%02X:%02X:%02X",
             addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
    if (s_inst) {
        ble_svc_gap_device_name_set(s_inst->deviceName());
        s_inst->startAdvertising();
    }
}

// host 被复位（异常路径，仅记日志）
static void onReset(int reason) {
    ESP_LOGW(TAG, "host reset, reason=%d", reason);
}

// NimBLE host 线程入口：跑协议栈主循环
static void hostTask(void*) {
    ESP_LOGI(TAG, "nimble host task start");
    nimble_port_run();  // 阻塞直至 nimble_port_stop()
    nimble_port_freertos_deinit();
}

// 初始化 NimBLE：起广播名、挂帧回调、注册 GATT 表、拉起 host 任务
bool GattServer::begin(const char* namePrefix) {
    s_inst = this;

    // 广播名 GAGA-XXXX：XXXX = BLE MAC 后四位（十六进制大写，与 Arduino 线同源）
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_BT);
    snprintf(name_, sizeof(name_), "%s-%02X%02X", namePrefix, mac[4], mac[5]);

    // 帧重组完成 → 业务回调；重组异常仅记日志（不致命）
    decoder_.onFrame([this](uint8_t type, const uint8_t* payload, uint16_t len) {
        if (frameCb_) frameCb_(type, payload, len);
    });
    decoder_.onError([](const char* reason) {
        ESP_LOGW(TAG, "frame decode error: %s", reason);
    });

    int rc = nimble_port_init();
    if (rc != 0) {
        ESP_LOGE(TAG, "nimble_port_init 失败 rc=%d", rc);
        return false;
    }

    ble_hs_cfg.sync_cb = onSync;
    ble_hs_cfg.reset_cb = onReset;
    // 不配对不绑定（明文链路，同 Arduino 线），无需 store 回调

    ble_svc_gap_init();
    ble_svc_gatt_init();

    rc = ble_gatts_count_cfg(g_svcs);
    if (rc == 0) rc = ble_gatts_add_svcs(g_svcs);
    // 不要显式调 ble_gatts_start()：host 启动时 ble_hs_start() 会自动调它
    // （ble_hs.c:808），重复调用会触发 gatts 内存释放重建（ble_gatts.c:3556 注释）。
    // 注意：val_handle 在 host 启动时才回填（异步），begin() 里读是 0——
    // 挪到 handleConnect 里赋值（2026-09-23 BLE 上行沉默的根因，见 docs/bug-analysis-0923.md）
    if (rc != 0) {
        ESP_LOGE(TAG, "gatts 注册失败 rc=%d", rc);
        return false;
    }
    ble_att_set_preferred_mtu(517);  // protocol.md §1：MTU 协商目标 517

    sendMtx_ = xSemaphoreCreateMutex();  // 发送互斥（重构：原 main.cpp 的裸锁下沉到这）

    nimble_port_freertos_init(hostTask);
    return true;
}

// 发一帧给 App（NUS 风格 TX notify）：按协商 MTU 切成若干物理包逐个 notify。
// 内部持锁：app 任务 / 上行泵 / talk 可能并发发送，NimBLE 不许两个任务同时进
bool GattServer::sendFrame(uint8_t type, const uint8_t* payload, uint16_t len) {
    if (sendMtx_ == nullptr || !connected_ || txValHandle_ == 0) {
        return false;  // 未连接/句柄未回填：静默拒发，调用方记丢弃
    }
    xSemaphoreTake(sendMtx_, portMAX_DELAY);
    unsigned packets_sent_this_call = 0;
    bool ok = true;
    // 物理包上限 = MTU - 3（ATT notify header），下限 20
    const size_t maxPacket = mtu_ > DEFAULT_MTU ? static_cast<size_t>(mtu_) - 3 : 20;
    auto packets = encodeFrame(type, payload, len, maxPacket);
    for (auto& pkt : packets) {
        os_mbuf* om = ble_hs_mbuf_from_flat(pkt.data(), pkt.size());
        if (om == nullptr) {
            ESP_LOGW(TAG, "mbuf 分配失败，丢弃本包");
            ok = false;
            break;
        }
        const int rc = ble_gatts_notify_custom(connHandle_, txValHandle_, om);
        if (rc != 0) {
            ESP_LOGW(TAG, "notify 失败 rc=%d", rc);
            ok = false;
            break;
        }
        // 节流改良（2026-09-25 talk 卡顿）：原每包 vTaskDelay(1)=10ms@100Hz tick，
        // 25 包/秒的音频被拖成贴地到达（播放队列恒 0，任何抖动即断粮）。
        // 改 yield + 每 4 包才真睡一拍：吞吐 ×4，仍防 notify 队列溢出
        if ((packets_sent_this_call++ & 3) == 3) vTaskDelay(1);
        else taskYIELD();
    }
    xSemaphoreGive(sendMtx_);
    return ok;
}

// JSON 信令包成 type=0x02 帧发出
bool GattServer::sendJson(const char* json) {
    return sendFrame(FRAME_TYPE_JSON,
                     reinterpret_cast<const uint8_t*>(json),
                     static_cast<uint16_t>(strlen(json)));
}

// App → 设备的写入数据转交帧重组器
void GattServer::handleRxWrite(const uint8_t* data, size_t len) {
    decoder_.feed(data, len);
}

// 连接建立：记句柄、回填 TX val_handle、丢弃残帧、通知上层
void GattServer::handleConnect(uint16_t connHandle) {
    connected_  = true;
    connHandle_ = connHandle;
    mtu_        = DEFAULT_MTU;  // 先按默认 23，MTU 协商事件随后修正
    // TX 特征句柄此刻已被 host 启动回填（begin 时读是 0，见 bug-analysis-0923）
    txValHandle_ = s_txValHandle;
    decoder_.reset();
    ESP_LOGI(TAG, "connected, conn=%u tx_handle=%u", connHandle, txValHandle_);
    if (connCb_) connCb_(true);
}

// 断连：清连接状态、丢残帧、通知上层、立刻恢复广播等 App 重连
void GattServer::handleDisconnect() {
    connected_  = false;
    connHandle_ = 0xFFFF;
    mtu_        = DEFAULT_MTU;
    decoder_.reset();  // 丢弃半个残帧
    if (connCb_) connCb_(false);
    startAdvertising();  // 断连后立刻恢复广播，等 App 退避重连
}

// 记下 MTU 协商结果——sendFrame 的分包大小由此决定
void GattServer::handleMtuChange(uint16_t mtu) {
    mtu_ = mtu;
    ESP_LOGI(TAG, "mtu negotiated: %u", mtu_);
}

}  // namespace gaga
