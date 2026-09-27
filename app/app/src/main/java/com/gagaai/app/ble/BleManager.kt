package com.gagaai.app.ble

import android.annotation.SuppressLint
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothGattDescriptor
import android.bluetooth.BluetoothGattService
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothProfile
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanFilter
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.PackageManager
import android.os.Build
import android.os.ParcelUuid
import com.gagaai.app.util.BridgeState
import com.gagaai.app.util.Prefs
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import java.util.UUID

/**
 * BLE 管理器：App 是 Central，主动扫描并连接 ESP32（GATT Server，广播名前缀 GAGA-）。
 *
 * 流程：扫描（Service UUID 过滤 + 名称前缀校验）→ 连接 → 发现服务 → 请求 MTU 517
 * → 订阅 TX(Notify) → 就绪。上行字节经 onUplink 回调原样交给 MQTT；
 * writeDownlink() 把服务器下发的字节按当前 MTU-3 分包写入 RX(Write)。
 *
 * 任何断连 / 失败都进入指数退避重扫（1s 起步，封顶 30s）。
 * 另有两条正交的保命通道：
 * - 扫描看门狗：SCANNING 35s 无任何结果就重启扫描（系统长时间扫描会静默降级/
 *   停发结果，尤其 ColorOS 后台），连续 3 次无结果退回退避重扫循环；
 * - 蓝牙开关监听：ACTION_STATE_CHANGED，蓝牙关闭时清理链路并显示等待，
 *   重新打开时立即重扫。
 */
@SuppressLint("MissingPermission") // 运行时权限由 MainActivity 申请，调用前用 hasBlePermission() 守卫
class BleManager(
    private val context: Context,
    private val scope: CoroutineScope,
    private val onUplink: (ByteArray) -> Unit,
) {
    /** BLE 链路就绪（服务发现 + Notify 订阅完成，含每次重连）：
     *  Service 借此把最新 MQTT 状态立刻推给设备（ADR-038 链路状态信令）。 */
    var onReady: (() -> Unit)? = null

    companion object {
        val SERVICE_UUID: UUID = UUID.fromString("6e400001-b5a3-f393-e0a9-e50e24dcca9e")
        val CHAR_TX_UUID: UUID = UUID.fromString("6e400003-b5a3-f393-e0a9-e50e24dcca9e") // Notify, 设备 → App
        val CHAR_RX_UUID: UUID = UUID.fromString("6e400002-b5a3-f393-e0a9-e50e24dcca9e") // Write, App → 设备
        val CCCD_UUID: UUID = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")

        const val NAME_PREFIX = "GAGA-"
        const val TARGET_MTU = 517
        const val DEFAULT_MTU = 23

        private const val MAX_RETRY_DELAY_MS = 30_000L

        /** MAC 直连连续失败这么多次后退回扫描发现（设备换了地址/不在） */
        private const val DIRECT_ATTEMPTS_MAX = 3

        /** 扫描看门狗：SCANNING 后这么久没任何结果就重启扫描（系统会静默降级/停发结果） */
        private const val SCAN_WATCHDOG_MS = 35_000L

        /** 连续看门狗重启这么多次仍无结果，退回带退避的完整重扫循环 */
        private const val WATCHDOG_LIMIT = 3

        /** 下行合批冲刷时限：攒不满 MTU 也最多等这么久（音频延迟上限 +12ms） */
        private const val BATCH_FLUSH_MS = 12L
    }

    private enum class Phase { IDLE, SCANNING, CONNECTING, READY }

    private val bluetoothManager: BluetoothManager? =
        context.getSystemService(BluetoothManager::class.java)

    @Volatile
    private var phase = Phase.IDLE
    private var running = false
    private var retryCount = 0
    private var retryJob: Job? = null

    // 扫描看门狗（与退避重连正交：治"扫描活着但永远没结果"的假死）
    private var watchdogJob: Job? = null
    private var watchdogCount = 0

    private var bluetoothStateRegistered = false

    private var gatt: BluetoothGatt? = null
    private var rxCharacteristic: BluetoothGattCharacteristic? = null

    @Volatile
    private var currentMtu = DEFAULT_MTU

    // 后台守株待兔连接（2026-09-27）：autoConnect=true 挂在系统蓝牙栈上，
    // 设备一出现栈自己连——ColorOS 限流不了它，App 后台也生效。设备重启/
    // 广播消失后手机后台扫不到、必须手动开 App 的根治方案。
    private var bgGatt: BluetoothGatt? = null

    // 下行写队列：BLE 写必须串行，等 onCharacteristicWrite 后再发下一包
    private val writeQueue = ArrayDeque<ByteArray>()
    private var writeInFlight = false
    private var writeInFlightAt = 0L      // 本次写入起跳时刻（守卫判卡死用）
    private var writeGuardJob: Job? = null

    // region 生命周期

    @Synchronized
    fun start() {
        if (running) return
        running = true
        retryCount = 0
        watchdogCount = 0
        registerBluetoothStateReceiver()
        if (!hasBlePermission()) {
            BridgeState.setBleStatus("Bluetooth permission missing")
            return
        }
        if (bluetoothManager?.adapter?.isEnabled != true) {
            BridgeState.setBleStatus("Bluetooth off, waiting")
            scheduleRetry()
            return
        }
        connectPreferDirect()
    }

    // region MAC 直连（ADR-025 的红利）

    private var directAttempts = 0

    /**
     * 优先按已记住的 MAC 直连（CDM 配对或任意一次连接成功后写入）——
     * 绕开 BLE 扫描：后台扫描会被系统限流/停发结果（ColorOS 尤甚，见文件头注释），
     * 而 connectGatt(已知MAC) 不受限。无 MAC 或连续 [DIRECT_ATTEMPTS_MAX] 次
     * 直连失败（设备换了 MAC/不在）退回扫描发现。
     */
    private fun connectPreferDirect() {
        if (!running || !hasBlePermission()) return
        Prefs.ensureLoaded(context)
        val mac = Prefs.pairedMac
        if (mac.isEmpty() || directAttempts >= DIRECT_ATTEMPTS_MAX) {
            startScan()
            return
        }
        directAttempts++
        try {
            val device = bluetoothManager?.adapter?.getRemoteDevice(mac)
            if (device == null) {
                startScan()
                return
            }
            phase = Phase.CONNECTING
            closeGatt()
            val name = Prefs.pairedName.ifEmpty { mac }
            BridgeState.setBleStatus("Direct connect $name… (try $directAttempts)")
            gatt = device.connectGatt(context, false, gattCallback, BluetoothDevice.TRANSPORT_LE)
        } catch (e: Exception) {
            BridgeState.log("Direct connect failed: ${e.message}, fallback to scan")
            startScan()
        }
    }

    @Synchronized
    fun stop() {
        running = false
        retryJob?.cancel()
        retryJob = null
        cancelWatchdog()
        unregisterBluetoothStateReceiver()
        stopScan()
        closeGatt()
        closeBgGatt()
        synchronized(writeQueue) {
            writeQueue.clear()
            writeInFlight = false
        }
        writeGuardJob?.cancel()
        phase = Phase.IDLE
        BridgeState.setBleStatus("Stopped")
    }

    // region 扫描

    private fun startScan() {
        if (!running || !hasBlePermission()) return
        if (bluetoothManager?.adapter?.isEnabled != true) {
            BridgeState.setBleStatus("Bluetooth off, waiting")
            scheduleRetry()
            return
        }
        val scanner = bluetoothManager?.adapter?.bluetoothLeScanner
        if (scanner == null) {
            BridgeState.setBleStatus("No BLE scanner available")
            scheduleRetry()
            return
        }
        phase = Phase.SCANNING
        BridgeState.setBleStatus("Scanning ($NAME_PREFIX*)…")
        val filter = ScanFilter.Builder()
            .setServiceUuid(ParcelUuid(SERVICE_UUID))
            .build()
        val settings = ScanSettings.Builder()
            .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
            .build()
        try {
            // 每次全新 callback 对象：部分机型（实测 ColorOS）复用同一 callback
            // 的扫描会话在后台被系统静默降级后不再回调，重建会话才恢复
            stopScan()
            activeScanCallback = object : ScanCallback() {
                override fun onScanResult(callbackType: Int, result: ScanResult) {
                    scanCallback.onScanResult(callbackType, result)
                }
            }
            scanner.startScan(listOf(filter), settings, activeScanCallback)
            armWatchdog()
        } catch (e: Exception) {
            BridgeState.setBleStatus("Scan start failed: ${e.message}")
            scheduleRetry()
        }
    }

    // App 回前台（Activity.onResume）：后台扫描被 ColorOS 限流是"卡在扫描中
    // 必须手动 Stop/Start"的根因——回前台立即重建扫描会话（前台不受限流），
    // 用户无需任何手动操作。仅在"正在扫/正等重试"时动作，已连接则不动。
    /** BLE 是否已就绪（已连接 + 通道可用）——伴生 kick 等调用方分流日志用。 */
    fun isReady(): Boolean = phase == Phase.READY

    fun onAppForeground() {
        if (!running) return
        if (phase == Phase.SCANNING) {
            BridgeState.log("App 前台：重建扫描会话（绕过后台限流）")
            retryCount = 0
            watchdogCount = 0
            startScan()
        } else if (phase == Phase.IDLE) {
            // 正在退避等待：提前触发重连
            retryJob?.cancel()
            retryJob = scope.launch {
                delay(300)
                if (running) connectPreferDirect()
            }
        }
    }

    // 伴生设备出现（系统 CDM 事件）：设备已在广播，跳过退避立即重连。
    // 已连接/正在连接则不动。
    fun kickReconnect() {
        if (!running || phase == Phase.READY || phase == Phase.CONNECTING) return
        BridgeState.log("Companion kick: 立即重连（跳过退避）")
        retryJob?.cancel()
        retryCount = 0
        directAttempts = 0
        scope.launch {
            delay(200)
            if (running) connectPreferDirect()
        }
    }

    private var activeScanCallback: ScanCallback? = null

    // region 扫描看门狗

    private fun armWatchdog() {
        watchdogJob?.cancel()
        watchdogJob = scope.launch {
            delay(SCAN_WATCHDOG_MS)
            if (running && phase == Phase.SCANNING) onScanWatchdog()
        }
    }

    private fun cancelWatchdog() {
        watchdogJob?.cancel()
        watchdogJob = null
    }

    private fun onScanWatchdog() {
        watchdogCount++
        stopScan()
        if (watchdogCount >= WATCHDOG_LIMIT) {
            // 连续重启扫描也没结果：走一次带退避的完整重扫循环
            watchdogCount = 0
            BridgeState.log("⏱ Scan watchdog: no result ${WATCHDOG_LIMIT} times, backing off")
            armBackgroundConnect()   // 后台扫描被限流的铁证：挂栈级守株待兔
            phase = Phase.IDLE
            scheduleRetry()
        } else {
            BridgeState.log("⏱ Scan watchdog: no result in ${SCAN_WATCHDOG_MS / 1000}s, restarting scan (try $watchdogCount)")
            startScan()
        }
    }

    private fun stopScan() {
        try {
            activeScanCallback?.let {
                bluetoothManager?.adapter?.bluetoothLeScanner?.stopScan(it)
            }
            bluetoothManager?.adapter?.bluetoothLeScanner?.stopScan(scanCallback)
        } catch (_: Exception) {
        }
        activeScanCallback = null
    }

    private val scanCallback = object : ScanCallback() {
        override fun onScanResult(callbackType: Int, result: ScanResult) {
            if (!running || phase != Phase.SCANNING) return
            val name = result.device.name
            val hasService = result.scanRecord?.serviceUuids?.contains(ParcelUuid(SERVICE_UUID)) == true
            val nameMatches = name?.startsWith(NAME_PREFIX) == true
            if (!nameMatches && !hasService) return
            cancelWatchdog()
            watchdogCount = 0
            stopScan()
            BridgeState.setBleStatus("Found ${name ?: result.device.address}, connecting…")
            connect(result.device)
        }

        override fun onScanFailed(errorCode: Int) {
            if (!running) return
            cancelWatchdog()
            BridgeState.setBleStatus("Scan failed (code=$errorCode), retrying")
            scheduleRetry()
        }
    }

    // region 蓝牙开关监听

    private val bluetoothStateReceiver = object : BroadcastReceiver() {
        override fun onReceive(ctx: Context, intent: Intent) {
            if (intent.action != BluetoothAdapter.ACTION_STATE_CHANGED) return
            val state = intent.getIntExtra(BluetoothAdapter.EXTRA_STATE, BluetoothAdapter.ERROR)
            when (state) {
                BluetoothAdapter.STATE_OFF -> {
                    cancelWatchdog()
                    stopScan()
                    closeGatt()
                    if (running) {
                        phase = Phase.IDLE
                        BridgeState.setBleStatus("Bluetooth off, waiting")
                        BridgeState.log("🔵 Bluetooth off, waiting")
                    }
                }
                BluetoothAdapter.STATE_ON -> {
                    if (running && phase != Phase.READY) {
                        BridgeState.log("🔵 Bluetooth on, scanning again")
                        retryCount = 0
                        watchdogCount = 0
                        startScan()
                    }
                }
            }
        }
    }

    private fun registerBluetoothStateReceiver() {
        if (bluetoothStateRegistered) return
        val filter = IntentFilter(BluetoothAdapter.ACTION_STATE_CHANGED)
        // 系统广播，无需 exported 标记；API 33+ 显式给 NOT_EXPORTED 以过 targetSdk 校验
        if (Build.VERSION.SDK_INT >= 33) {
            context.registerReceiver(bluetoothStateReceiver, filter, Context.RECEIVER_NOT_EXPORTED)
        } else {
            context.registerReceiver(bluetoothStateReceiver, filter)
        }
        bluetoothStateRegistered = true
    }

    private fun unregisterBluetoothStateReceiver() {
        if (!bluetoothStateRegistered) return
        try {
            context.unregisterReceiver(bluetoothStateReceiver)
        } catch (_: Exception) {
        }
        bluetoothStateRegistered = false
    }

    // region 连接与 GATT

    private fun connect(device: BluetoothDevice) {
        phase = Phase.CONNECTING
        closeGatt()
        closeBgGatt()   // 前台快路径连上在即，守株待兔退役（防双连接）
        gatt = device.connectGatt(context, false, gattCallback, BluetoothDevice.TRANSPORT_LE)
    }

    /** 挂后台守株待兔（幂等）：只在扫描看门狗退避（后台扫描已被限流）时挂。 */
    fun armBackgroundConnect() {
        if (!running || phase == Phase.READY || bgGatt != null) return
        Prefs.ensureLoaded(context)
        val mac = Prefs.pairedMac
        if (mac.isEmpty()) return
        try {
            val device = bluetoothManager?.adapter?.getRemoteDevice(mac) ?: return
            bgGatt = device.connectGatt(context, true, gattCallback,
                                         BluetoothDevice.TRANSPORT_LE)
            BridgeState.log("👻 后台守株待兔已挂（设备出现即自动连）")
        } catch (e: Exception) {
            BridgeState.log("后台守株待兔挂载失败: ${e.message}")
        }
    }

    private fun closeBgGatt() {
        try {
            bgGatt?.disconnect()
            bgGatt?.close()
        } catch (_: Exception) {
        }
        bgGatt = null
    }

    private fun closeGatt() {
        try {
            gatt?.disconnect()
            gatt?.close()
        } catch (_: Exception) {
        }
        gatt = null
        rxCharacteristic = null
        currentMtu = DEFAULT_MTU
        synchronized(writeQueue) {
            writeQueue.clear()
            writeInFlight = false
        }
        writeGuardJob?.cancel()
        synchronized(batchBuf) {
            batchBuf.reset()
            batchJob?.cancel()
            batchJob = null
        }
    }

    private val gattCallback = object : BluetoothGattCallback() {

        override fun onConnectionStateChange(g: BluetoothGatt, status: Int, newState: Int) {
            when (newState) {
                BluetoothProfile.STATE_CONNECTED -> {
                    if (g !== gatt && g === bgGatt) {
                        // 守株待兔命中：后台连接先到 → 晋升为主连接，
                        // 收掉前台扫描/重试/旧主连接
                        BridgeState.log("👻 后台守株待兔命中，晋升主连接")
                        try { gatt?.disconnect(); gatt?.close() } catch (_: Exception) {}
                        gatt = g
                        bgGatt = null
                        cancelWatchdog()
                        stopScan()
                        retryJob?.cancel()
                    }
                    BridgeState.setBleStatus("Connected, discovering services…")
                    if (!g.discoverServices()) {
                        BridgeState.setBleStatus("Failed to start service discovery")
                        handleLinkDropped()
                    }
                }
                BluetoothProfile.STATE_DISCONNECTED -> {
                    BridgeState.setBleStatus("Device disconnected (status=$status)")
                    BridgeState.log("BLE lost (status=$status)")
                    handleLinkDropped()
                }
            }
        }

        override fun onServicesDiscovered(g: BluetoothGatt, status: Int) {
            if (status != BluetoothGatt.GATT_SUCCESS) {
                BridgeState.setBleStatus("Service discovery failed (status=$status)")
                handleLinkDropped()
                return
            }
            val service: BluetoothGattService? = g.getService(SERVICE_UUID)
            val tx = service?.getCharacteristic(CHAR_TX_UUID)
            val rx = service?.getCharacteristic(CHAR_RX_UUID)
            if (service == null || tx == null || rx == null) {
                BridgeState.setBleStatus("GaGa service traits missing")
                handleLinkDropped()
                return
            }
            rxCharacteristic = rx
            BridgeState.setBleStatus("Negotiating MTU $TARGET_MTU…")
            if (!g.requestMtu(TARGET_MTU)) {
                // 个别机型 requestMtu 返回 false，退回默认 MTU 继续
                currentMtu = DEFAULT_MTU
                enableTxNotify(g, tx)
            }
        }

        override fun onMtuChanged(g: BluetoothGatt, mtu: Int, status: Int) {
            currentMtu = if (status == BluetoothGatt.GATT_SUCCESS) mtu else DEFAULT_MTU
            val tx = g.getService(SERVICE_UUID)?.getCharacteristic(CHAR_TX_UUID)
            if (tx == null) {
                handleLinkDropped()
                return
            }
            enableTxNotify(g, tx)
        }

        private fun enableTxNotify(g: BluetoothGatt, tx: BluetoothGattCharacteristic) {
            if (!g.setCharacteristicNotification(tx, true)) {
                BridgeState.setBleStatus("Notify subscribe failed")
                handleLinkDropped()
                return
            }
            val cccd = tx.getDescriptor(CCCD_UUID)
            if (cccd == null) {
                BridgeState.setBleStatus("CCCD descriptor missing")
                handleLinkDropped()
                return
            }
            val ok = if (Build.VERSION.SDK_INT >= 33) {
                g.writeDescriptor(cccd, BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE) ==
                    BluetoothGatt.GATT_SUCCESS
            } else {
                @Suppress("DEPRECATION")
                cccd.setValue(BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE)
                @Suppress("DEPRECATION")
                g.writeDescriptor(cccd)
            }
            if (!ok) {
                BridgeState.setBleStatus("CCCD write failed")
                handleLinkDropped()
            }
        }

        override fun onDescriptorWrite(g: BluetoothGatt, descriptor: BluetoothGattDescriptor, status: Int) {
            if (descriptor.uuid != CCCD_UUID) return
            if (status == BluetoothGatt.GATT_SUCCESS) {
                phase = Phase.READY
                retryCount = 0
                BridgeState.setBleStatus("Connected ${g.device.name ?: g.device.address} (MTU=$currentMtu)")
                BridgeState.log("BLE ready, forwarding")
                // 链路真正可用才记 MAC：直连重连免扫描（后台扫描被 ColorOS 限流）。
                // 不依赖 CDM 配对——扫描/直连连上就记，配对只是额外的系统保活通道。
                try {
                    Prefs.savePairedDevice(context, g.device.address, g.device.name ?: "GAGA")
                } catch (_: Exception) {
                }
                onReady?.invoke()  // 就绪即推 MQTT 链路状态（ADR-038）
            } else {
                BridgeState.setBleStatus("CCCD write failed (status=$status)")
                handleLinkDropped()
            }
        }

        // Android 13+ 新回调
        override fun onCharacteristicChanged(
            g: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic,
            value: ByteArray,
        ) {
            if (characteristic.uuid == CHAR_TX_UUID) onUplink(value)
        }

        // Android 12 及以下旧回调
        @Suppress("DEPRECATION", "OVERRIDE_DEPRECATION")
        override fun onCharacteristicChanged(g: BluetoothGatt, characteristic: BluetoothGattCharacteristic) {
            if (characteristic.uuid == CHAR_TX_UUID) {
                characteristic.value?.let(onUplink)
            }
        }

        override fun onCharacteristicWrite(
            g: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic,
            status: Int,
        ) {
            if (status != BluetoothGatt.GATT_SUCCESS) {
                BridgeState.log("BLE write failed (status=$status), packet dropped")
            }
            synchronized(writeQueue) {
                writeInFlight = false
            }
            pumpWriteQueue()
        }
    }

    // region 下行写入

    // 下行合批（2026-09-26 实时音频延迟根治）：服务器的 ogg 分片小（~100-300B）
    // 而一次 BLE 写一轮连接事件——ColorOS 50ms 间隔下逐片写只有 ~20 片/s，
    // 音频到达速率 > 写出速率 → 积压 → "文字秒到、音频等半分钟"。
    // 攒到 ~MTU 或 12ms 即冲刷：设备端 FrameDecoder 是流式重组器，
    // 一次写入含多帧拼接天然支持（feed 逐字节推进，完整帧逐个回调）。
    private val batchBuf = java.io.ByteArrayOutputStream()
    private var batchDeadline = 0L
    private var batchJob: Job? = null

    /** 服务器下行字节：先进合批缓冲，攒满/到期再按安全块写入。 */
    fun writeDownlink(data: ByteArray) {
        if (phase != Phase.READY || rxCharacteristic == null) {
            BridgeState.log("BLE not ready, dropped ${data.size}B downlink")
            return
        }
        // 写入块封顶 240B（2026-09-27 实锤）：设备侧协商 MTU=517，但 ColorOS
        // 的栈拒绝 514B 写（IllegalArgumentException 确定性复现，OEM 内部
        // 上限与协商值不一致）。240 是所有 OEM 都安全的经典值；音频下行
        // 吞吐按 240B/写 @20ms 节奏仍远超实时
        val chunkSize = (currentMtu - 3).coerceAtLeast(20).coerceAtMost(240)
        synchronized(batchBuf) {
            batchBuf.write(data)
            val buffered = batchBuf.size()
            if (buffered >= chunkSize) {
                flushBatchLocked(chunkSize)
                return
            }
            if (batchJob == null) {
                batchDeadline = System.currentTimeMillis() + BATCH_FLUSH_MS
                batchJob = scope.launch {
                    val wait = (batchDeadline - System.currentTimeMillis()).coerceAtLeast(0)
                    delay(wait)
                    synchronized(batchBuf) {
                        flushBatchLocked(chunkSize)
                        batchJob = null
                    }
                }
            }
        }
        ensureBatchGuard()
    }

    /**
     * 合批常驻守卫（v0.4.11）：每 50ms 兜底冲刷一次非空缓冲。不再把正确性
     * 押在 12ms 一次性定时器协程的存活上——2026-09-27 真机实锤：514B 写
     * 异常后（即便被 catch）出现"帧滞留缓冲静默丢失"（小包直写路径通、
     * 合批路径零到达），定时器协程一旦没跑到收尾就永久滞留。守卫在，
     * 缓冲最多滞留 50ms。
     */
    private var batchGuardJob: Job? = null
    private fun ensureBatchGuard() {
        if (batchGuardJob?.isActive == true) return
        batchGuardJob = scope.launch {
            while (true) {
                delay(50)
                val cs = (currentMtu - 3).coerceAtLeast(20).coerceAtMost(240)
                synchronized(batchBuf) {
                    if (batchBuf.size() > 0 && phase == Phase.READY &&
                        rxCharacteristic != null) {
                        batchJob?.cancel()
                        batchJob = null
                        flushBatchLocked(cs)
                    }
                }
            }
        }
    }

    /** 必须持 batchBuf 锁调用：把缓冲切成 MTU-3 包入写队列并冲刷 */
    private fun flushBatchLocked(chunkSize: Int) {
        batchJob?.cancel()
        batchJob = null
        if (batchBuf.size() == 0) return
        val data = batchBuf.toByteArray()
        batchBuf.reset()
        var offset = 0
        val chunks = mutableListOf<ByteArray>()
        while (offset < data.size) {
            val end = (offset + chunkSize).coerceAtMost(data.size)
            chunks.add(data.copyOfRange(offset, end))
            offset = end
        }
        synchronized(writeQueue) {
            writeQueue.addAll(chunks)
        }
        pumpWriteQueue()
    }

    /**
     * App 本地信令：不经服务器、由 App 自己组帧发给设备（ADR-038 链路状态推送）。
     * 帧格式与服务端 encode_frame 严格一致（protocol.md §2）：
     *   [1B type=0x02(JSON)][2B payload 长度 big-endian][payload]
     * JSON 载荷 ~30B，恒为单包帧（type 不置 MORE 位），复用既有写队列串行发出。
     * 哑管道纪律不受影响：仍只"组这一种本地帧"，不解析任何转发内容。
     */
    fun writeLocalSignal(json: String) {
        // 静默退出改为留痕（2026-09-26 排查教训：下行链路任何一跳哑掉
        // 都无日志可查）：link 信令是链路心跳，发不出去必须可见
        if (phase != Phase.READY || rxCharacteristic == null) {
            BridgeState.log("⚠ local signal dropped: BLE not ready (phase=$phase)")
            return
        }
        val payload = json.toByteArray(Charsets.UTF_8)
        val frame = ByteArray(payload.size + 3)
        frame[0] = 0x02                             // FRAME_TYPE_JSON
        frame[1] = ((payload.size shr 8) and 0xFF).toByte()
        frame[2] = (payload.size and 0xFF).toByte()
        payload.copyInto(frame, 3)
        synchronized(writeQueue) {
            writeQueue.add(frame)
        }
        pumpWriteQueue()
    }

    private fun pumpWriteQueue() {
        val g = gatt ?: return
        val rx = rxCharacteristic ?: return
        val packet: ByteArray
        synchronized(writeQueue) {
            if (writeInFlight) return
            val next = writeQueue.removeFirstOrNull() ?: return
            writeInFlight = true
            writeInFlightAt = android.os.SystemClock.elapsedRealtime()
            packet = next
        }
        ensureWriteGuard()
        val ok = try {
            if (Build.VERSION.SDK_INT >= 33) {
                g.writeCharacteristic(rx, packet, BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT) ==
                    BluetoothGatt.GATT_SUCCESS
            } else {
                @Suppress("DEPRECATION")
                rx.writeType = BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
                @Suppress("DEPRECATION")
                rx.value = packet
                @Suppress("DEPRECATION")
                g.writeCharacteristic(rx)
            }
        } catch (e: IllegalArgumentException) {
            // Android 13+ 的新 writeCharacteristic 对"值超过当前 MTU-3"不返回
            // 错误码而是直接抛异常——2026-09-27 真机闪退实锤（合批 514B 大包
            // 撞上协商窗口期的 23 MTU，异常从 HiveMQ 回调线程炸出 = 进程死）。
            // 自愈：缓存 MTU 按本包大小减半降档（后续合批自动变小），本包丢弃
            // ——信令类由设备对账协议补，丢包有账可查
            BridgeState.log("⚠ BLE 写超长（${packet.size}B），MTU 缓存降档自愈")
            currentMtu = maxOf(23, (packet.size / 2) + 3).coerceAtMost(currentMtu)
            false
        }
        if (!ok) {
            BridgeState.log("BLE write call failed, packet dropped")
            synchronized(writeQueue) {
                writeInFlight = false
            }
            pumpWriteQueue()
        }
    }

    /**
     * 写队列常驻守卫（v0.4.6，替代 v0.4.3 的逐写布防）：每 2s 自查一次
     * writeInFlight 是否超过 5s 没被回调清掉——是就强制放行并打日志。
     * 逐写布防的漏洞：任何路径让 writeInFlight 卡 true 而没走到布防
     *（竞态/协程异常），看门狗就永远不存在，下行静默死到重连
     *（2026-09-26 晚真机实锤：上行正常、broker 在投递，设备却一个
     * 字节都收不到，且全链路零错误日志）。常驻守卫与写入路径解耦，
     * 不依赖"每次都记得布防"。
     */
    private fun ensureWriteGuard() {
        if (writeGuardJob?.isActive == true) return
        writeGuardJob = scope.launch {
            while (true) {
                delay(2000)
                val since = synchronized(writeQueue) {
                    if (writeInFlight) writeInFlightAt else 0L
                }
                if (since != 0L &&
                    android.os.SystemClock.elapsedRealtime() - since > 5000) {
                    BridgeState.log("⏱ BLE write 卡死 >5s，强制继续队列")
                    synchronized(writeQueue) {
                        writeInFlight = false
                    }
                    pumpWriteQueue()
                }
            }
        }
    }

    // region 重连

    private fun handleLinkDropped() {
        closeGatt()
        if (!running) {
            phase = Phase.IDLE
            return
        }
        scheduleRetry()
    }

    private fun scheduleRetry() {
        if (!running) return
        retryJob?.cancel()
        val delayMs = (1000L shl retryCount.coerceAtMost(5)).coerceAtMost(MAX_RETRY_DELAY_MS)
        retryCount++
        phase = Phase.IDLE
        BridgeState.setBleStatus("Retrying in ${delayMs / 1000}s…")
        retryJob = scope.launch {
            delay(delayMs)
            // 重连优先 MAC 直连（ADR-025：免扫描），直连续败后由 connectPreferDirect 退回扫描
            if (running) connectPreferDirect()
        }
    }

    private fun hasBlePermission(): Boolean {
        return if (Build.VERSION.SDK_INT >= 31) {
            context.checkSelfPermission(android.Manifest.permission.BLUETOOTH_SCAN) ==
                PackageManager.PERMISSION_GRANTED &&
                context.checkSelfPermission(android.Manifest.permission.BLUETOOTH_CONNECT) ==
                PackageManager.PERMISSION_GRANTED
        } else {
            context.checkSelfPermission(android.Manifest.permission.ACCESS_FINE_LOCATION) ==
                PackageManager.PERMISSION_GRANTED
        }
    }
}
