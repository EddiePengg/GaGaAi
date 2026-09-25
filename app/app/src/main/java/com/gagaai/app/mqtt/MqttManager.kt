package com.gagaai.app.mqtt

import com.gagaai.app.util.BridgeState
import com.gagaai.app.util.FrameLogger
import com.hivemq.client.mqtt.MqttClient
import com.hivemq.client.mqtt.datatypes.MqttQos
import com.hivemq.client.mqtt.mqtt3.Mqtt3AsyncClient
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicLong
import kotlin.concurrent.thread

/**
 * MQTT 客户端封装：哑管道的一端（进程级单例）。
 * 上行：publish gaga/up（QoS 1，不 retain）
 * 下行：subscribe gaga/down（QoS 1）
 * 断线由 HiveMQ automaticReconnect 做指数退避重连（3s 起步，封顶 2 分钟）。
 *
 * 生命周期纪律（修复重复下行 bug）：
 * - 同一时刻只允许一个 client 活着：start()/stop() 递增 generation，
 *   旧 client 的一切回调（连接/断开/订阅/publish 完成）到达时若 gen 过期直接丢弃；
 * - 旧 client 的 disconnect 在后台线程最多等 [DISCONNECT_WAIT_MS]，确保订阅真正销毁；
 * - 订阅只在首次 connect 成功后做一次。HiveMQ 自动重连默认
 *   resubscribeIfSessionExpired=true，会自己恢复订阅；在 connected 回调里反复订阅
 *   会在同一 client 的 flow tree 里叠加重复回调（每重连一次多一份下行）。
 *
 * 为什么是 object 单例（修复 MQTT 乒乓互踢，ADR-039）：
 * 之前 MqttManager 随 GagaService 每次重建而新建。服务被系统快速反复重建时
 * （ColorOS 停服务 + START_STICKY 重拉），每个新实例都 build 一个新的 HiveMQ
 * 客户端连向同一 broker，而旧实例的 DISCONNECT 包在 VPN 隧道抖动下可能送不干净
 * （retire 最多等 3s 就放弃），于是同一 Client ID 的多个连接并发存在、互相
 * "session taken over"，每秒互踢成乒乓；清理后台重开（进程重建清零实例）即恢复。
 * 单例 + start() 幂等（同地址直接复用现有连接）从结构上杜绝多实例并发。
 */
object MqttManager {
    const val TOPIC_UP = "gaga/up"
    const val TOPIC_DOWN = "gaga/down"

    private const val DISCONNECT_WAIT_MS = 3000L

    /** 下行字节去向（服务里接到 BLE 写口），服务每次 onCreate 重设 */
    @Volatile
    var onDownlink: ((ByteArray) -> Unit)? = null

    /** MQTT 连/断回调（ADR-038：推送设备链路状态），服务每次 onCreate 重设 */
    @Volatile
    var onStateChanged: ((Boolean) -> Unit)? = null

    /** Client ID（基于 ANDROID_ID），服务每次 onCreate 重设 */
    @Volatile
    var clientId: String = ""

    private val generation = AtomicLong(0)
    private var client: Mqtt3AsyncClient? = null

    @Volatile
    private var connected = false

    @Volatile
    private var lastHost = ""

    @Volatile
    private var lastPort = -1

    @Synchronized
    fun start(host: String, port: Int) {
        // 幂等：地址没变且客户端还在（连接中或已连）→ 直接复用，不叠加并发连接
        if (client != null && host == lastHost && port == lastPort) return
        val gen = generation.incrementAndGet()
        retire(client)
        client = null
        connected = false
        lastHost = host
        lastPort = port
        if (host.isBlank()) {
            BridgeState.setMqttStatus("No broker configured")
            return
        }
        BridgeState.setMqttStatus("Connecting $host:$port")
        val c = MqttClient.builder()
            .useMqttVersion3()
            .identifier(clientId)
            .serverHost(host)
            .serverPort(port)
            // keepalive 默认 60s，与协议一致；重连退避 3s 起步（默认 1s 在服务
            // 反复重建等场景会放大同 ID 互踢的乒乓烈度）
            .automaticReconnect()
            .initialDelay(3, TimeUnit.SECONDS)
            .maxDelay(2, TimeUnit.MINUTES)
            .applyAutomaticReconnect()
            .addConnectedListener {
                if (!isCurrent(gen)) return@addConnectedListener
                connected = true
                BridgeState.setMqttStatus("Connected $host:$port")
                BridgeState.log("MQTT connected")
                onStateChanged?.invoke(true)
            }
            .addDisconnectedListener { ctx ->
                if (!isCurrent(gen)) return@addDisconnectedListener
                connected = false
                val cause = ctx.cause.message ?: ctx.cause.javaClass.simpleName
                BridgeState.setMqttStatus("Disconnected (retrying) $cause")
                BridgeState.log("MQTT lost: $cause")
                onStateChanged?.invoke(false)
            }
            .buildAsync()
        client = c
        c.connect().whenComplete { _, error ->
            if (!isCurrent(gen)) return@whenComplete
            if (error != null) {
                BridgeState.setMqttStatus("Connect failed: ${error.message ?: error.javaClass.simpleName} (retrying)")
            } else {
                subscribeDownlink(gen, c)
            }
        }
    }

    private fun subscribeDownlink(gen: Long, c: Mqtt3AsyncClient) {
        c.subscribeWith()
            .topicFilter(TOPIC_DOWN)
            .qos(MqttQos.AT_LEAST_ONCE)
            .callback { publish ->
                if (!isCurrent(gen)) return@callback
                val bytes = publish.payloadAsBytes
                FrameLogger.downlink(bytes)
                onDownlink?.invoke(bytes)
            }
            .send()
            .whenComplete { _, error ->
                if (!isCurrent(gen)) return@whenComplete
                if (error != null) {
                    BridgeState.log("Subscribe $TOPIC_DOWN failed: ${error.message ?: error.javaClass.simpleName}")
                } else {
                    BridgeState.log("Subscribed $TOPIC_DOWN")
                }
            }
    }

    /** BLE 上行字节原样发布到 gaga/up。 */
    fun publishUplink(data: ByteArray) {
        val c = client ?: return
        if (!connected) {
            BridgeState.log("MQTT down, dropped ${data.size}B uplink")
            return
        }
        FrameLogger.uplink(data)
        c.publishWith()
            .topic(TOPIC_UP)
            .qos(MqttQos.AT_LEAST_ONCE)
            .payload(data)
            .send()
            .whenComplete { _, error ->
                if (error != null && c === client) {
                    BridgeState.log("Publish failed: ${error.message ?: error.javaClass.simpleName}")
                }
            }
    }

    @Synchronized
    fun stop() {
        generation.incrementAndGet()
        connected = false
        val c = client
        client = null
        lastHost = ""
        lastPort = -1
        retire(c)
    }

    private fun isCurrent(gen: Long): Boolean = gen == generation.get()

    /** 作废旧 client：后台线程断开并最多等 [DISCONNECT_WAIT_MS]，保证连接与订阅真正销毁。 */
    private fun retire(c: Mqtt3AsyncClient?) {
        if (c == null) return
        thread(name = "mqtt-retire", isDaemon = true) {
            try {
                c.disconnect().get(DISCONNECT_WAIT_MS, TimeUnit.MILLISECONDS)
            } catch (_: Exception) {
            }
        }
    }
}
