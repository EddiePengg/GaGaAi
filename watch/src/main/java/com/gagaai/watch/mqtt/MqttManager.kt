package com.gagaai.watch.mqtt

import com.gagaai.watch.proto.Frame
import com.hivemq.client.mqtt.MqttClient
import com.hivemq.client.mqtt.MqttGlobalPublishFilter
import com.hivemq.client.mqtt.datatypes.MqttQos
import com.hivemq.client.mqtt.mqtt3.Mqtt3AsyncClient
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicLong
import kotlin.concurrent.thread
import kotlin.math.min

/**
 * 手表侧 MQTT 客户端（object 单例）。
 *
 * 与手机端 `com.gagaai.app.mqtt.MqttManager` 同源，但**去掉了 BLE/服务层耦合**——
 * 手表没有 BLE，本身就是一台"嘎嘎设备"，直接讲 MQTT。
 *
 * 沿用 ADR-044 的三条铁律：
 * 1. **不用 HiveMQ automaticReconnect**：它埋在 client 上无法事后摘除，锁屏
 *    掐网时 disconnect 超时会让旧 client 自己复活，与新 client 同 ID 互踢。
 *    改为 supervisor 线程 connect→订阅→等断线→指数退避重连（3s→2min）。
 * 2. **代际（generation）**：start()/stop() 递增 gen，旧 client 的一切回调到达
 *    时若 gen 过期直接丢弃，保证同一时刻只有一个 client 能生效。
 * 3. **下行全局监听只注册一次**：`publishes(ALL)` 在 builder 时挂一次，之后
 *    重连的 subscribe 不带 callback，避免 HiveMQ 叠加注册导致"每重连一次
 *    就多收一份下行"。
 */
object MqttManager {
    const val TOPIC_UP = "gaga/up"
    const val TOPIC_DOWN = "gaga/down"

    private const val RETRY_DELAY_MS = 3000L
    private const val RETRY_MAX_MS = 120_000L
    private const val CONNECT_WAIT_MS = 30_000L
    private const val DISCONNECT_WAIT_MS = 3000L

    /** 下行字节（JSON 信令 + 下行音频帧），主线程之外回调，调用方自己切线程 */
    @Volatile
    var onDownlink: ((ByteArray) -> Unit)? = null

    /** 连/断回调，用于 UI 状态条 */
    @Volatile
    var onStateChanged: ((Boolean) -> Unit)? = null

    /** 调试日志回调（UI 显示 / logcat） */
    @Volatile
    var onLog: ((String) -> Unit)? = null

    /** MQTT Client ID，必须是 `watch-<稳定 id>`——同 ID 会互踢 */
    @Volatile
    var clientId: String = "watch-unknown"

    private val generation = AtomicLong(0)
    private var client: Mqtt3AsyncClient? = null

    @Volatile
    private var connected = false

    @Volatile
    private var lastHost = ""

    @Volatile
    private var lastPort = -1

    @Volatile
    private var lastUser = ""

    @Volatile
    private var lastPass = ""

    @Volatile
    private var lastDownlinkAt = AtomicLong(0)

    /** 上行丢弃日志的节流时刻（2026-09-26：刷屏降频） */
    @Volatile
    private var lastDropLogAt = 0L

    /**
     * 连接 broker。username 留空 = 匿名（局域网无鉴权 broker 兼容）；
     * 非空则走 MQTT SimpleAuth（ADR-061：公网暴露必须开鉴权）。
     * 幂等判断含凭据——地址没变但密码换了，也必须重建连接。
     */
    @Synchronized
    fun start(host: String, port: Int, username: String = "", password: String = "") {
        if (client != null && host == lastHost && port == lastPort && username == lastUser) return
        val gen = generation.incrementAndGet()
        retire(client)
        client = null
        connected = false
        lastHost = host
        lastPort = port
        lastUser = username
        lastPass = password
        if (host.isBlank()) {
            onLog?.invoke("MQTT: 未配置 broker")
            return
        }
        onLog?.invoke("MQTT: 连接 $host:$port${if (username.isNotBlank()) "（鉴权 $username）" else ""}")
        val builder = MqttClient.builder()
            .useMqttVersion3()
            .identifier(clientId)
            .serverHost(host)
            .serverPort(port)
        if (username.isNotBlank()) {
            builder.simpleAuth()
                .username(username)
                .password(password.toByteArray(Charsets.UTF_8))
                .applySimpleAuth()
        }
        val c = builder
            // keepalive 15s：智能手表熄屏更激进，60s 静默窗口会被系统当空闲回收
            .addConnectedListener {
                if (!isCurrent(gen)) return@addConnectedListener
                connected = true
                onLog?.invoke("MQTT: 已连接")
                onStateChanged?.invoke(true)
            }
            .addDisconnectedListener { ctx ->
                if (!isCurrent(gen)) return@addDisconnectedListener
                connected = false
                val cause = ctx.cause.message ?: ctx.cause.javaClass.simpleName
                onLog?.invoke("MQTT: 断开（重连中）$cause")
                onStateChanged?.invoke(false)
            }
            .buildAsync()
        c.publishes(MqttGlobalPublishFilter.ALL) { publish ->
            lastDownlinkAt.set(System.currentTimeMillis())
            if (!isCurrent(gen)) {
                onLog?.invoke("⚠ 下行到达但代际过期被丢弃")
                return@publishes
            }
            onDownlink?.invoke(publish.payloadAsBytes)
        }
        client = c
        lastDownlinkAt.set(0)
        thread(name = "watch-mqtt-supervisor", isDaemon = true) { supervise(gen, c) }
    }

    /**
     * 下行僵尸自愈（同 ADR-044 手机端）：连接"看着健康"但 publishes 回调
     * 长期不被调用（服务端每 30s 发 hello_ack 心跳），90s 无下行 = 判死重建。
     */
    init {
        thread(name = "watch-mqtt-downlink-watchdog", isDaemon = true) {
            while (true) {
                Thread.sleep(10_000)
                val c = client ?: continue
                if (!connected) continue
                val last = lastDownlinkAt.get()
                if (last == 0L) continue
                if (System.currentTimeMillis() - last > 90_000) {
                    onLog?.invoke("🔄 下行僵尸 >90s，重建客户端")
                    val host = lastHost
                    val port = lastPort
                    val user = lastUser
                    val pass = lastPass
                    stop()
                    if (host.isNotBlank()) start(host, port, user, pass)
                }
            }
        }
    }

    private fun supervise(gen: Long, c: Mqtt3AsyncClient) {
        var backoff = RETRY_DELAY_MS
        while (isCurrent(gen)) {
            try {
                c.connectWith().keepAlive(15).send()
                    .get(CONNECT_WAIT_MS, TimeUnit.MILLISECONDS)
            } catch (e: Exception) {
                if (!isCurrent(gen)) return
                onLog?.invoke("MQTT: 连接失败 ${e.message ?: e.javaClass.simpleName}（重试）")
                sleepUninterruptible(backoff)
                backoff = min(backoff * 2, RETRY_MAX_MS)
                continue
            }
            if (!isCurrent(gen)) return
            connected = true
            subscribeDownlink(gen, c)
            backoff = RETRY_DELAY_MS
            while (isCurrent(gen) && connected) {
                Thread.sleep(500)
            }
            if (!isCurrent(gen)) return
            sleepUninterruptible(backoff)
            backoff = min(backoff * 2, RETRY_MAX_MS)
        }
    }

    private fun sleepUninterruptible(millis: Long) {
        try {
            Thread.sleep(millis)
        } catch (_: InterruptedException) {
            Thread.currentThread().interrupt()
        }
    }

    private fun subscribeDownlink(gen: Long, c: Mqtt3AsyncClient) {
        c.subscribeWith()
            .topicFilter(TOPIC_DOWN)
            .qos(MqttQos.AT_LEAST_ONCE)
            .send()
            .whenComplete { _, error ->
                if (!isCurrent(gen)) return@whenComplete
                if (error != null) {
                    onLog?.invoke("订阅 $TOPIC_DOWN 失败：${error.message ?: error.javaClass.simpleName}")
                } else {
                    lastDownlinkAt.set(System.currentTimeMillis())
                    onLog?.invoke("已订阅 $TOPIC_DOWN")
                    flushOffAir()   // 订阅就绪 = 链路双向可用，离线帧可以补了
                }
            }
    }

    /** 发一帧上行（JSON 信令或 Opus 音频帧，调用方自己用 [Frame.encode] 组包）。 */
    fun publishUplink(frameBytes: ByteArray) {
        val c = client ?: return
        if (!connected) {
            // v0.2.1 断链不丢话（手表版 ADR-049）：断开期间的帧按序存 RAM，
            // 重连后原样冲出（rec_start 在缓冲里保持时序，服务端会话语义完整）。
            // 上限 6000 帧 ≈ 2 分钟音频，超限丢最旧（对齐设备端 8 分钟 PSRAM
            // 缓存的哲学：宁可丢最早的，最新的照说照存）。
            synchronized(offAir) {
                offAir.addLast(frameBytes)
                while (offAir.size > MAX_OFFAIR_FRAMES) offAir.removeFirst()
            }
            val now = System.currentTimeMillis()
            if (now - lastDropLogAt > 3000) {
                lastDropLogAt = now
                onLog?.invoke("MQTT 未连接，帧入离线缓冲（${offAir.size} 帧，重连后自动补发）")
            }
            return
        }
        c.publishWith()
            .topic(TOPIC_UP)
            .qos(MqttQos.AT_LEAST_ONCE)
            .payload(frameBytes)
            .send()
            .whenComplete { _, error ->
                if (error != null && c === client) {
                    onLog?.invoke("上行发送失败：${error.message ?: error.javaClass.simpleName}")
                }
            }
    }

    private val offAir = ArrayDeque<ByteArray>()
    private const val MAX_OFFAIR_FRAMES = 6000   // ≈2 分钟

    /**
     * 重连成功后调用：按序冲出离线缓冲。10ms/帧限速（6000 帧最长 60s），
     * 冲到一半又断线：未发部分塞回缓冲**头部**保持时序，等下次重连。
     */
    private fun flushOffAir() {
        val toSend = synchronized(offAir) {
            ArrayList(offAir).also { offAir.clear() }
        }
        if (toSend.isEmpty()) return
        thread(name = "watch-offair-flush", isDaemon = true) {
            onLog?.invoke("↩ 补发离线帧 ${toSend.size} 个")
            var i = 0
            while (i < toSend.size) {
                val c = client
                if (c == null || !connected) {
                    synchronized(offAir) {
                        for (j in toSend.size - 1 downTo i) offAir.addFirst(toSend[j])
                    }
                    onLog?.invoke("补发中断（剩 ${toSend.size - i} 帧），等下次重连")
                    return@thread
                }
                c.publishWith().topic(TOPIC_UP).qos(MqttQos.AT_LEAST_ONCE)
                    .payload(toSend[i]).send()
                i++
                Thread.sleep(10)
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

    private fun retire(c: Mqtt3AsyncClient?) {
        if (c == null) return
        thread(name = "watch-mqtt-retire", isDaemon = true) {
            try {
                c.disconnect().get(DISCONNECT_WAIT_MS, TimeUnit.MILLISECONDS)
            } catch (_: Exception) {
                onLog?.invoke("旧连接清理失败（无害）")
            }
        }
    }
}
