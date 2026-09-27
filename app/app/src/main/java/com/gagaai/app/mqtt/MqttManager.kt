package com.gagaai.app.mqtt

import com.gagaai.app.util.BridgeState
import com.gagaai.app.util.FrameLogger
import com.hivemq.client.mqtt.MqttClient
import com.hivemq.client.mqtt.MqttGlobalPublishFilter
import com.hivemq.client.mqtt.datatypes.MqttQos
import com.hivemq.client.mqtt.mqtt3.Mqtt3AsyncClient
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicLong
import kotlin.concurrent.thread
import kotlin.math.min

/**
 * MQTT 客户端封装：哑管道的一端（进程级单例）。
 * 上行：publish gaga/up（QoS 1，不 retain）
 * 下行：subscribe gaga/down（QoS 1）
 *
 * 重连归本类自管（ADR-044）：**不用 HiveMQ automaticReconnect**。
 * 乒乓根因（v0.4.2 修复）：retire() 断开旧 client 最多等 3s 就放弃，而放弃时旧
 * client 身上的 automaticReconnect 仍活着——锁屏时段 ColorOS 掐网，disconnect
 * 极易超时失败；网络一恢复旧 client 自己复活，与新 client 同 Client ID 互踢
 * （session taken over），3 秒一轮永不收敛（服务端日志实锤：重连间隔恰为
 * automaticReconnect 的 initialDelay=3s）。automaticReconnect 是"埋在 client
 * 身上、无法事后摘除"的机制，所以根治 = 一开始就不装它：
 * - 每个 client 配一条唯一的 mqtt-supervisor 线程做 connect→订阅→等断线→
 *   退避重连（同一个 client 对象，指数退避 3s→2min）；
 * - 被 generation 作废的 client 断线后永远不会再自己发起连接——即使 retire
 *   断开失败，它也只是占着一条旧连接，同 ID 下次 connect 时会被 broker 踢掉，
 *   不可能再踢别人。
 *
 * 生命周期纪律：
 * - 同一时刻只允许一个 client 活着：start()/stop() 递增 generation，旧 client
 *   的一切回调（连接/断开/下行）到达时若 gen 过期直接丢弃；
 * - 下行监听用 publishes(ALL) 全局回调、每个 client 只注册一次；以后每次重连
 *   重新 subscribe 都不带 callback——否则 HiveMQ 的 subscribeWith().callback{}
 *   会叠加注册，每重连一次就多收一份下行（v0.3 实际踩过的坑）。
 * - 为什么是 object 单例（ADR-039）：服务被系统快速反复重建时，实例级封装会让
 *   多个 HiveMQ 客户端并发连向同一 broker；单例 + start() 幂等（同地址直接
 *   复用现有连接）从结构上杜绝多实例并发。
 */
object MqttManager {
    const val TOPIC_UP = "gaga/up"
    const val TOPIC_DOWN = "gaga/down"

    private const val RETRY_DELAY_MS = 3000L
    private const val RETRY_MAX_MS = 120_000L
    private const val CONNECT_WAIT_MS = 30_000L
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

    @Volatile
    private var lastUser = ""

    @Volatile
    private var lastPass = ""

    /**
     * 连接 broker。username 留空 = 匿名（局域网无鉴权 broker 兼容）；
     * 非空走 MQTT SimpleAuth（ADR-061：公网暴露必须开鉴权）。
     * 幂等判断含凭据——地址没变但密码换了，也必须重建连接。
     */
    @Synchronized
    fun start(host: String, port: Int, username: String = "", password: String = "") {
        // 幂等：地址+凭据没变且客户端还在（连接中或已连）→ 直接复用，不叠加并发连接
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
            BridgeState.setMqttStatus("No broker configured")
            return
        }
        BridgeState.setMqttStatus("Connecting $host:$port")
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
            // keepalive 15s（v0.4.9，2026-09-26）：ColorOS 熄屏会收割"空闲"
            // 后台 socket（WiFi 高性能锁都拦不住，实锤），60s 心跳留下的
            // 静默窗口正好被收割——压到 15s 让连接永远"有最近活动"。
            // 刻意不配 automaticReconnect（类注释，ADR-044）
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
        // 全局下行监听只注册这一次；之后每轮重连的 subscribe 都不带 callback，
        // 不会在 client 内叠加回调（重复下行坑，见类注释）
        c.publishes(MqttGlobalPublishFilter.ALL) { publish ->
            // 心跳时刻先行更新（在代际检查之前）：回调被调用本身就证明下行
            // 通道活着——僵尸检测的判据就是"连接着但回调长期不被调用"
            lastDownlinkAt.set(System.currentTimeMillis())
            if (!isCurrent(gen)) {
                // 可观测（2026-09-26 僵尸连接排查）：下行到达却被代际丢弃，
                // 是"上行活下行死"的头号嫌疑路径，发生时必须留痕
                BridgeState.log("⚠ 下行到达但代际过期被丢弃（client gen=$gen）")
                return@publishes
            }
            val bytes = publish.payloadAsBytes
            FrameLogger.downlink(bytes)
            onDownlink?.invoke(bytes)
        }
        client = c
        lastDownlinkAt.set(0)  // 新客户端从零起判：等首帧下行再开始计时
        thread(name = "mqtt-supervisor", isDaemon = true) { supervise(gen, c) }
    }

    // ---- 下行僵尸自愈（2026-09-26 真机实锤的故障形态）----
    // 症状：连接"健康"（上行发布正常、broker 侧在投递），但 publishes 回调
    // 长期不被调用 → 设备收不到任何下行，App 状态栏全绿，零错误日志。
    // 根因悬置在 HiveMQ 客户端内部（keepalive 超时重连若干轮后出现），
    // 自愈不依赖根因：服务端每 30s 发一帧 hello_ack 心跳，"已连接却 90s
    // 无任何下行回调" = 判死，整个 client 推倒重建（stop+start 同地址）。
    private val lastDownlinkAt = java.util.concurrent.atomic.AtomicLong(0)

    init {
        thread(name = "mqtt-downlink-watchdog", isDaemon = true) {
            while (true) {
                Thread.sleep(10_000)
                val c = client ?: continue
                if (!connected) continue
                val last = lastDownlinkAt.get()
                if (last == 0L) continue  // 还没收到过任何下行：等首帧
                // 45s（v0.4.12 收紧，原 90s）：服务端心跳 30s 一跳，健康连接
                // 不可能 45s 无下行。重建成本 ≈2s，误杀无害——把熄屏僵尸期的
                // 黄卡等待窗从最长 100s 压到 ~55s
                if (System.currentTimeMillis() - last > 45_000) {
                    BridgeState.log("🔄 MQTT 下行僵尸 >45s，强制重建客户端")
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

    /**
     * 一条 client 的全部重连事务：connect → 订阅 → 等断线 → 退避后重连同一对象。
     * gen 作废即退出（retire 断开失败也没关系：本线程退出后没人再连它）。
     */
    private fun supervise(gen: Long, c: Mqtt3AsyncClient) {
        var backoff = RETRY_DELAY_MS
        while (isCurrent(gen)) {
            try {
                // keepAlive 15s（v0.4.9）：ColorOS 熄屏收割"空闲"后台 socket
                //（WiFi 高性能锁都拦不住，实锤），60s 心跳的静默窗口正好被
                // 收割——压到 15s 让连接永远"有最近活动"。keepAlive 在
                // HiveMQ 是连接参数（connectWith），不在 client builder 上
                c.connectWith().keepAlive(15).send()
                    .get(CONNECT_WAIT_MS, TimeUnit.MILLISECONDS)
            } catch (e: Exception) {
                if (!isCurrent(gen)) return
                val cause = e.message ?: e.javaClass.simpleName
                BridgeState.setMqttStatus("Connect failed: $cause (retrying)")
                sleepUninterruptible(backoff)
                backoff = min(backoff * 2, RETRY_MAX_MS)
                continue
            }
            if (!isCurrent(gen)) return
            // connected listener 也会置位；这里显式兜底，防 listener 尚未回调时
            // 下面的轮询误判"已断线"而空转重连
            connected = true
            subscribeDownlink(gen, c)
            backoff = RETRY_DELAY_MS
            // 等断线：disconnected listener 置 connected=false。轮询而非阻塞等待，
            // 免掉 latch 与 listener 的注册竞态
            while (isCurrent(gen) && connected) {
                Thread.sleep(500)
            }
            if (!isCurrent(gen)) return
            // 断线状态已由 listener 汇报；退避后重连
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
                    BridgeState.log("Subscribe $TOPIC_DOWN failed: ${error.message ?: error.javaClass.simpleName}")
                } else {
                    // 订阅落定 = 下行僵尸计时的起跑线（90s 内必有心跳到）
                    lastDownlinkAt.set(System.currentTimeMillis())
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

    /**
     * 作废旧 client：后台线程断开并最多等 [DISCONNECT_WAIT_MS]。
     * 断不干净也安全（ADR-044）：该 client 没有 automaticReconnect，永远不会再
     * 自己连上来；占着的旧连接会在同 ID 下次 connect 时被 broker 踢掉。
     */
    private fun retire(c: Mqtt3AsyncClient?) {
        if (c == null) return
        thread(name = "mqtt-retire", isDaemon = true) {
            try {
                c.disconnect().get(DISCONNECT_WAIT_MS, TimeUnit.MILLISECONDS)
            } catch (_: Exception) {
                BridgeState.log("MQTT old connection cleanup failed (harmless)")
            }
        }
    }
}
