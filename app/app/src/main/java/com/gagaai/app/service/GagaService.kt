package com.gagaai.app.service

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.Build
import android.os.IBinder
import android.provider.Settings
import com.gagaai.app.MainActivity
import com.gagaai.app.ble.BleManager
import com.gagaai.app.mqtt.MqttManager
import com.gagaai.app.util.BridgeState
import com.gagaai.app.util.Prefs
import com.gagaai.app.util.UiStyle
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel

/**
 * 前台服务：哑管道的宿主。BLE 管理器和 MQTT 客户端都活在这里，
 * 各自负责断线自动重连（指数退避）。
 *
 * 保活组合拳（ADR-024）：START_STICKY + 常驻通知状态灯 + 看门狗闹钟
 * （KeepAlive）+ onTaskRemoved 自愈 + 开机自启（BootReceiver）+ 打开 App 自拉
 * （MainActivity）+ 伴生设备出现自拉（GagaCompanionService）。
 * 用户明确"停止服务"= Prefs.userStopped，所有自愈通道让位，直到重新打开 App。
 */
class GagaService : Service() {

    companion object {
        const val ACTION_START = "com.gagaai.app.action.START"
        const val ACTION_STOP = "com.gagaai.app.action.STOP"
        const val ACTION_BROKER_CHANGED = "com.gagaai.app.action.BROKER_CHANGED"
        const val ACTION_SEND_WIFI = "com.gagaai.app.action.SEND_WIFI"

        private const val CHANNEL_ID = "gaga_bridge"  // 新 id：旧渠道重要性不可改
        private const val NOTIFICATION_ID = 1

        /** 进程内的服务存活标记：看门狗据此决定要不要拉起（进程死=自动归 false） */
        @Volatile
        var isRunning = false
            private set

        fun startIntent(context: Context) = Intent(context, GagaService::class.java).setAction(ACTION_START)
        fun stopIntent(context: Context) = Intent(context, GagaService::class.java).setAction(ACTION_STOP)
    }

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main)
    private var bleManager: BleManager? = null

    /** MQTT 最新状态（null=尚未连过）。连/断即刻经 BLE 推给设备（ADR-038）。 */
    @Volatile
    private var mqttUp: Boolean? = null

    /** 把当前 MQTT 状态组成本地信令推给设备（BLE 未就绪时静默跳过——
     *  下次 BLE 就绪或状态再变化时会补推）。 */
    private fun pushLinkState() {
        val up = mqttUp ?: return
        bleManager?.writeLocalSignal("{\"type\":\"link\",\"mqtt\":$up}")
    }

    /** 通知跟随 BLE/MQTT 状态刷新——通知栏就是链路状态灯（一眼判断要不要开 App） */
    private val stateListener = object : BridgeState.Listener {
        override fun onStateChanged(state: BridgeState.Snapshot) {
            updateNotification(state)
        }
    }

    override fun onCreate() {
        super.onCreate()
        Prefs.ensureLoaded(this)
        isRunning = true
        createNotificationChannel()
        val state = BridgeState.current()
        BridgeState.setServiceRunning(true)
        BridgeState.addListener(stateListener)
        startForegroundWithNotification(state)
        KeepAlive.armWatchdog(this)  // 保活第二条命：15 分钟看门狗自续期
        BridgeState.log("Service started")

        // MqttManager 是进程级单例（ADR-039）：服务重建不再新建客户端，
        // 杜绝同一 Client ID 多连接并发互踢（session taken over 乒乓）
        MqttManager.onDownlink = { bytes ->
            bleManager?.writeDownlink(bytes)
        }
        MqttManager.onStateChanged = { up ->
            // 链路状态信令（ADR-038）：MQTT 连/断即刻推送设备——设备按键预检
            // 据此提示"手机没连上服务器"，杜绝对着空气说话
            mqttUp = up
            pushLinkState()
        }
        MqttManager.clientId = clientId()
        val ble = BleManager(this, scope) { bytes ->
            MqttManager.publishUplink(bytes)
        }
        ble.onReady = { pushLinkState() }  // 每次 BLE 就绪先同步当前 MQTT 状态
        bleManager = ble

        MqttManager.start(Prefs.brokerHost, Prefs.brokerPort)
        ble.start()
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        when (intent?.action) {
            ACTION_STOP -> {
                // 用户明确停止：所有自愈通道让位（看门狗/开机/任务移除都不复活）
                Prefs.setUserStopped(this, true)
                KeepAlive.cancelWatchdog(this)
                stopSelf()
                return START_NOT_STICKY
            }
            ACTION_SEND_WIFI -> {
                // WiFi 凭证下发（ADR-039）：本地信令帧经 BLE 直达设备
                val json = intent?.getStringExtra("json") ?: ""
                if (json.isNotEmpty()) bleManager?.writeLocalSignal(json)
            }
            ACTION_BROKER_CHANGED -> {
                Prefs.load(this)
                BridgeState.log("Broker changed, reconnecting MQTT")
                // 单例 start 幂等：同地址直接复用现有连接，不同地址才重建
                MqttManager.start(Prefs.brokerHost, Prefs.brokerPort)
            }
        }
        // START / 系统重启服务（intent 为 null）都保持运行
        return START_STICKY
    }

    override fun onTaskRemoved(rootIntent: Intent?) {
        // 手滑清除多任务卡片 = 进程随时被补刀：立即复活 + 闹钟兜底（ADR-024）
        if (!Prefs.userStopped) {
            BridgeState.log("Task removed, self-healing…")
            KeepAlive.reviveAfterTaskRemoved(this)
        }
        super.onTaskRemoved(rootIntent)
    }

    override fun onDestroy() {
        isRunning = false
        BridgeState.removeListener(stateListener)
        bleManager?.stop()
        MqttManager.stop()
        bleManager = null
        scope.cancel()
        BridgeState.setServiceRunning(false)
        BridgeState.setBleStatus("-")
        BridgeState.setMqttStatus("-")
        BridgeState.log("Service stopped")
        super.onDestroy()
    }

    override fun onBind(intent: Intent?): IBinder? = null

    private fun clientId(): String {
        val androidId = Settings.Secure.getString(contentResolver, Settings.Secure.ANDROID_ID) ?: "unknown"
        return "app-$androidId"
    }

    private fun createNotificationChannel() {
        // DEFAULT 重要性 + 静音：状态栏常驻可见（LOW 在 ColorOS 上会被折叠成"静默通知"）
        val channel = NotificationChannel(
            CHANNEL_ID,
            "gaga ai bridge service",
            NotificationManager.IMPORTANCE_DEFAULT,
        ).apply {
            description = "Persistent BLE ↔ MQTT bridge status in the notification shade"
            setSound(null, null)
            enableVibration(false)
            setShowBadge(false)
        }
        getSystemService(NotificationManager::class.java).createNotificationChannel(channel)
    }

    private fun startForegroundWithNotification(state: BridgeState.Snapshot) {
        try {
            startForeground(
                NOTIFICATION_ID,
                buildNotification(state),
                ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE,
            )
        } catch (e: SecurityException) {
            // Android 14+：connectedDevice 类型要求蓝牙运行时权限已授予。
            // 数据清除/重装后权限重置，看门狗/伴生回调仍会在后台拉起本服务，
            // 不能让异常炸掉进程（否则每 15 分钟崩一次），静默退出等用户授权。
            BridgeState.log("Start failed: BT permission missing — open app to grant")
            stopSelf()
        }
    }

    private fun updateNotification(state: BridgeState.Snapshot) {
        if (!isRunning) return
        getSystemService(NotificationManager::class.java)
            .notify(NOTIFICATION_ID, buildNotification(state))
    }

    /** 折叠态单行：BLE ✅ · MQTT ✅（渲染规则见 UiStyle） */
    private fun compactStatus(state: BridgeState.Snapshot): String =
        "BLE ${UiStyle.mark(state.bleStatus)} · MQTT ${UiStyle.mark(state.mqttStatus)}"

    /** 展开态：逐行带详情 */
    private fun detailStatus(state: BridgeState.Snapshot): String =
        "BLE ${UiStyle.mark(state.bleStatus)} ${UiStyle.detail(state.bleStatus)}\n" +
            "MQTT ${UiStyle.mark(state.mqttStatus)} ${UiStyle.detail(state.mqttStatus)}"

    private fun buildNotification(state: BridgeState.Snapshot): Notification {        val openIntent = PendingIntent.getActivity(
            this,
            0,
            Intent(this, MainActivity::class.java),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
        )
        return Notification.Builder(this, CHANNEL_ID)
            .setContentTitle("GaGaAI Listening")
            .setContentText(compactStatus(state))
            .setStyle(Notification.BigTextStyle().bigText(detailStatus(state)))
            .setSmallIcon(android.R.drawable.stat_sys_data_bluetooth)
            .setContentIntent(openIntent)
            .setOngoing(true)
            .setShowWhen(false)
            .build()
    }
}
