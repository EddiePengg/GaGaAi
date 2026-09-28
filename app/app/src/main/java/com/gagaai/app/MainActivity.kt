package com.gagaai.app

import android.animation.ObjectAnimator
import android.Manifest
import android.app.AlertDialog
import android.app.Activity
import android.app.PendingIntent
import android.bluetooth.BluetoothDevice
import android.content.BroadcastReceiver
import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.companion.AssociationRequest
import android.companion.CompanionDeviceManager
import android.content.Intent
import android.content.IntentFilter
import android.content.IntentSender
import android.content.pm.PackageManager
import android.net.wifi.WifiManager
import android.graphics.Color
import android.graphics.drawable.GradientDrawable
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.os.PowerManager
import android.provider.Settings
import android.view.View
import android.view.WindowInsets
import android.widget.Button
import android.widget.EditText
import android.widget.ScrollView
import android.widget.Switch
import android.widget.TextView
import android.widget.Toast
import com.gagaai.app.service.GagaService
import com.gagaai.app.service.KeepAlive
import com.gagaai.app.util.BridgeState
import com.gagaai.app.util.KeepAlivePermission
import com.gagaai.app.util.Prefs
import com.gagaai.app.util.UiStyle

class MainActivity : Activity(), BridgeState.Listener {

    private lateinit var tvServiceStatus: TextView
    private lateinit var tvBleStatus: TextView
    private lateinit var tvMqttStatus: TextView
    private lateinit var tvOverall: TextView
    private lateinit var nodeService: View
    private lateinit var nodeBle: View
    private lateinit var nodeMqtt: View
    private lateinit var badgeService: TextView
    private lateinit var badgeBle: TextView
    private lateinit var badgeMqtt: TextView
    private lateinit var btnSettings: TextView
    private lateinit var imgDuck: View
    private lateinit var cardTerminal: View
    private lateinit var tvLog: TextView
    private lateinit var logScroll: ScrollView
    private lateinit var btnClearLog: Button
    private lateinit var btnToggleService: Button
    private lateinit var rowBattery: View
    private lateinit var rowCompanion: View
    private lateinit var batteryState: TextView
    private lateinit var companionState: TextView
    private lateinit var tvPairStatus: TextView
    private lateinit var pageScroll: ScrollView
    private lateinit var serverStatusRow: View
    private lateinit var serverStatusDot: TextView
    private lateinit var serverStatusText: TextView

    // 已渲染状态：行数 + 最后一行内容，用于增量追加并检测环形缓冲回卷
    private var renderedLogCount = -1
    private var renderedLastLine: String? = null

    /** 最新一次 render 的状态快照：整体状态徽章点击出修复指引时用 */
    @Volatile
    private var lastState: BridgeState.Snapshot? = null

    /** 配对时是否为断开蓝牙停了服务：配对结束（无论成败）都要拉回来 */
    private var pairingPausedService = false

    /** 鸭子摇摆动画（服务运行时开跳） */
    private var duckDance: ObjectAnimator? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        tvServiceStatus = findViewById(R.id.tvServiceStatus)
        tvBleStatus = findViewById(R.id.tvBleStatus)
        tvMqttStatus = findViewById(R.id.tvMqttStatus)
        tvOverall = findViewById(R.id.tvOverall)
        nodeService = findViewById(R.id.nodeService)
        nodeBle = findViewById(R.id.nodeBle)
        nodeMqtt = findViewById(R.id.nodeMqtt)
        badgeService = findViewById(R.id.badgeService)
        badgeBle = findViewById(R.id.badgeBle)
        badgeMqtt = findViewById(R.id.badgeMqtt)
        btnSettings = findViewById(R.id.btnSettings)
        imgDuck = findViewById(R.id.imgDuck)
        cardTerminal = findViewById(R.id.cardTerminal)
        tvLog = findViewById(R.id.tvLog)
        // 长按日志区 = 整段复制到剪贴板（排障时把日志带给开发者的唯一通道；
        // 不用 setTextIsSelectable——选择手势和嵌套 ScrollView 滚动打架）
        tvLog.setOnLongClickListener {
            val text = tvLog.text.toString()
            if (text.isBlank()) {
                Toast.makeText(this, "日志还是空的", Toast.LENGTH_SHORT).show()
            } else {
                val cm = getSystemService(ClipboardManager::class.java)
                cm.setPrimaryClip(ClipData.newPlainText("gaga log", text))
                val lines = text.count { it == '\n' } + 1
                Toast.makeText(this, "日志已复制（$lines 行）", Toast.LENGTH_SHORT).show()
            }
            true
        }
        logScroll = findViewById(R.id.logScroll)
        btnClearLog = findViewById(R.id.btnClearLog)
        btnToggleService = findViewById(R.id.btnToggleService)
        rowBattery = findViewById(R.id.rowBattery)
        rowCompanion = findViewById(R.id.rowCompanion)
        batteryState = findViewById(R.id.batteryState)
        companionState = findViewById(R.id.companionState)
        tvPairStatus = findViewById(R.id.tvPairStatus)
        pageScroll = findViewById(R.id.pageScroll)
        serverStatusRow = findViewById(R.id.serverStatusRow)
        serverStatusDot = findViewById(R.id.serverStatusDot)
        serverStatusText = findViewById(R.id.serverStatusText)

        Prefs.ensureLoaded(this)

        // 整体状态徽章：点击出修复指引
        tvOverall.setOnClickListener { showFixGuide() }

        // 齿轮：App 内设置（终端日志显隐 + 系统权限入口）
        btnSettings.setOnClickListener { showSettingsDialog() }

        // 服务器卡：主屏只显示状态；服务器配置与嘎嘎配网都在设置对话框里
        //（2026-09-27 用户定稿：主屏不堆表单，点状态行直达设置）
        serverStatusRow.setOnClickListener { showSettingsDialog() }

        btnClearLog.setOnClickListener {
            BridgeState.clearLogs()
        }

        btnToggleService.setOnClickListener {
            if (BridgeState.current().serviceRunning) {
                startService(GagaService.stopIntent(this))
                render(BridgeState.current().copy(serviceRunning = false))
            } else {
                if (!hasRuntimePermissions()) {
                    requestRuntimePermissions()
                }
                Prefs.setUserStopped(this, false)
                startBridgeService()
            }
        }

        // 保活①：电池白名单（Doze/ColorOS 后台管控的第一道豁免）
        rowBattery.setOnClickListener { requestBatteryWhitelist() }

        // 保活②/伴生：未配对时点击直接配对；已配对时弹管理菜单（重新配对/取消配对）
        rowCompanion.setOnClickListener {
            if (Prefs.pairedMac.isEmpty()) {
                startCompanionPairing()
            } else {
                AlertDialog.Builder(this)
                    .setTitle("Companion: ${Prefs.pairedName.ifEmpty { "GaGa" }}")
                    .setMessage("Paired ${Prefs.pairedMac}\nSystem wakes the bridge whenever GaGa shows up")
                    .setPositiveButton("Re-pair") { _, _ -> startCompanionPairing() }
                    .setNegativeButton("Unpair") { _, _ -> unpair() }
                    .setNeutralButton("Close", null)
                    .show()
            }
        }
        renderPairStatus()

        // 保活③：打开 App = 明确使用意图 → 自动拉起服务（也是"手滑关掉"的秒恢复路径）
        Prefs.setUserStopped(this, false)
        startBridgeService()

        requestRuntimePermissions()

        // 保活④：进 App 检查后台权限（onboarding 一次）；之后被系统杀只记日志不弹窗
        KeepAlivePermission.logSystemKillsIfAny(this)
        KeepAlivePermission.maybeGuide(this)
        // 保活⑤：重新注册伴生设备"出现"观察（实现也在 KeepAlive，开机/看门狗复活时同样会重打）
        KeepAlive.observeCompanionPresence(this)

        applyTerminalVisibility()


        // Android 15 强制 edge-to-edge：内容画进状态栏/挖孔底下，statusBarColor 失效。
        // 头部顶部间距 = 状态栏+挖孔的实际高度（动态注入），底部避让手势条——
        // 否则 "GaGaAI" 标题会被前置摄像头挡住、齿轮和电池图标重叠。
        val header = findViewById<View>(R.id.header)
        val contentArea = findViewById<View>(R.id.contentArea)
        pageScroll.setOnApplyWindowInsetsListener { v, insets ->
            val bars = if (Build.VERSION.SDK_INT >= 30) {
                insets.getInsets(
                    WindowInsets.Type.statusBars() or
                        WindowInsets.Type.displayCutout() or
                        WindowInsets.Type.navigationBars()
                )
            } else {
                @Suppress("DEPRECATION")
                null // 旧系统：内容默认不进状态栏，xml 的静态间距已足够
            }
            if (bars != null) {
                header.setPadding(header.paddingLeft, bars.top + dp(8), header.paddingRight, header.paddingBottom)
                contentArea.setPadding(
                    contentArea.paddingLeft, contentArea.paddingTop,
                    contentArea.paddingRight, bars.bottom + dp(8)
                )
            }
            insets
        }
    }

    /** 终端日志卡显隐：隐藏时日志照常在后台记录，只是不渲染；重新显示时全量重放并吸底 */
    private fun applyTerminalVisibility() {
        cardTerminal.visibility = if (Prefs.showTerminal) View.VISIBLE else View.GONE
        if (Prefs.showTerminal) {
            renderedLogCount = -1
            renderedLastLine = null
            renderLogs(BridgeState.current().logs)
        }
    }

    /** WiFi 列表选择的授权回调挂起点（授权后自动重试选择） */
    private var pendingWifiPick: ((String) -> Unit)? = null

    /** 手机当前连接的 SSID（读不到返回 null——不抛错不阻塞，字段留空手填） */
    private fun currentWifiSsid(): String? = try {
        if (Build.VERSION.SDK_INT >= 23 && checkSelfPermission(Manifest.permission.ACCESS_FINE_LOCATION)
            != PackageManager.PERMISSION_GRANTED) null
        else @Suppress("DEPRECATION") getSystemService(WifiManager::class.java)
            ?.connectionInfo?.ssid
            ?.removeSurrounding("\"")
            ?.takeIf { it.isNotEmpty() && it != "<unknown ssid>" }
    } catch (_: Exception) { null }

    /**
     * WiFi 列表单选。只读系统缓存的后台扫描结果（WifiManager.scanResults），
     * 绝不主动 startScan + 等广播——旧版"列表加载卡死"就是同步扫描死等的锅。
     * 无定位权限时申请一次，拒绝过就走手动输入（字段本身就是可编辑的）。
     */
    private fun pickWifiSsid(onPick: (String) -> Unit) {
        // 官方规则（developer.android.com wifi-permissions，2026-09-27 核实）：
        // targetSdk 33+ 调 getScanResults() 需 NEARBY_WIFI_DEVICES（运行时），
        // 且要拿未涂黑的 SSID 必须叠加 ACCESS_FINE_LOCATION——两个都要。
        // neverForLocation 标志不能加（加了 SSID/BSSID 会被系统涂黑）
        val wanted = buildList {
            add(Manifest.permission.ACCESS_FINE_LOCATION)
            if (Build.VERSION.SDK_INT >= 33) {
                add(Manifest.permission.NEARBY_WIFI_DEVICES)
            }
        }
        val missing = wanted.filter {
            Build.VERSION.SDK_INT < 23 ||
                checkSelfPermission(it) != PackageManager.PERMISSION_GRANTED
        }
        android.util.Log.d("gaga.wifi", "列表按钮：missing=$missing sdk=${Build.VERSION.SDK_INT}")
        if (missing.isNotEmpty()) {
            pendingWifiPick = onPick
            requestPermissions(missing.toTypedArray(), REQ_PICK_WIFI)
            Toast.makeText(this, "需要权限读 WiFi 列表（拒绝也可手动输入名称）", Toast.LENGTH_LONG).show()
            return
        }
        val names = scanSsidNames()
        android.util.Log.d("gaga.wifi", "缓存扫描结果：${names.size} 个 SSID")
        if (names.isEmpty()) {
            // 缓存被清过的兜底：异步触发一次扫描 + 3s 后回读，绝不阻塞等待
            Toast.makeText(this, "正在扫描 WiFi…", Toast.LENGTH_SHORT).show()
            val wm = getSystemService(WifiManager::class.java)
            if (wm != null) {
                try {
                    @Suppress("DEPRECATION")
                    val ok = wm.startScan()
                    android.util.Log.d("gaga.wifi", "主动 startScan=$ok（false=被节流，等缓存）")
                } catch (e: Exception) {
                    android.util.Log.w("gaga.wifi", "startScan 异常: ${e.message}")
                }
            }
            android.os.Handler(mainLooper).postDelayed({
                val retry = scanSsidNames()
                android.util.Log.d("gaga.wifi", "补扫回读：${retry.size} 个 SSID")
                if (retry.isEmpty()) {
                    Toast.makeText(this, "没读到 WiFi 列表，可直接手动输入名称", Toast.LENGTH_LONG).show()
                } else {
                    showWifiChoice(retry, onPick)
                }
            }, 3000)
            return
        }
        showWifiChoice(names, onPick)
    }

    private fun scanSsidNames(): List<String> = try {
        @Suppress("DEPRECATION")
        getSystemService(WifiManager::class.java)?.scanResults.orEmpty()
            .asSequence()
            .map { it.SSID.removeSurrounding("\"") }
            .filter { it.isNotEmpty() && it != "<unknown ssid>" }
            .distinct()
            .sorted()
            .toList()
    } catch (_: Exception) { emptyList() }

    private fun showWifiChoice(names: List<String>, onPick: (String) -> Unit) {
        AlertDialog.Builder(this)
            .setTitle("选择 WiFi")
            .setItems(names.toTypedArray()) { _, which -> onPick(names[which]) }
            .setNegativeButton("取消", null)
            .show()
    }

    private fun showSettingsDialog() {
        val view = layoutInflater.inflate(R.layout.dialog_settings, null)

        // ---- 📡 服务器（MQTT）：主屏迁入的 broker 配置（2026-09-27）----
        val etBrokerHost = view.findViewById<EditText>(R.id.etBrokerHost)
        val etBrokerPort = view.findViewById<EditText>(R.id.etBrokerPort)
        val etMqttUser = view.findViewById<EditText>(R.id.etMqttUser)
        val etMqttPass = view.findViewById<EditText>(R.id.etMqttPass)
        etBrokerHost.setText(Prefs.brokerHost)
        etBrokerPort.setText(Prefs.brokerPort.toString())
        etMqttUser.setText(Prefs.mqttUser)
        etMqttPass.setText(Prefs.mqttPass)
        view.findViewById<Button>(R.id.btnSaveBroker).setOnClickListener {
            val host = etBrokerHost.text.toString().trim()
            val port = etBrokerPort.text.toString().toIntOrNull() ?: Prefs.DEFAULT_PORT
            Prefs.saveBroker(this, host, port,
                etMqttUser.text.toString(), etMqttPass.text.toString())
            Toast.makeText(this, "Saved $host:${Prefs.brokerPort}", Toast.LENGTH_SHORT).show()
            if (BridgeState.current().serviceRunning) {
                startService(
                    Intent(this, com.gagaai.app.service.GagaService::class.java)
                        .setAction(com.gagaai.app.service.GagaService.ACTION_BROKER_CHANGED))
            }
        }

        // ---- 🦆 嘎嘎配网（家模式）：WiFi + 服务器配置一起下发 ----
        val etWifiSsid = view.findViewById<EditText>(R.id.etWifiSsid)
        val etWifiPass = view.findViewById<EditText>(R.id.etWifiPass)
        // SSID 预填手机当前连接的网络（在家配家模式，十有八九就是它）
        val currentSsid = currentWifiSsid()
        if (currentSsid != null && etWifiSsid.text.isBlank()) etWifiSsid.setText(currentSsid)
        view.findViewById<Button>(R.id.btnPickWifi).setOnClickListener {
            pickWifiSsid { ssid -> etWifiSsid.setText(ssid) }
        }
        view.findViewById<Button>(R.id.btnSendWifi).setOnClickListener {
            val ssid = etWifiSsid.text.toString().trim()
            if (ssid.isEmpty()) {
                Toast.makeText(this, "WiFi 名称不能为空", Toast.LENGTH_SHORT).show()
                return@setOnClickListener
            }
            // MQTT 配置显式随行（用户 2026-09-27 要求）：服务器区块的值 > 已存 Prefs
            val host = etBrokerHost.text.toString().trim().ifEmpty { Prefs.brokerHost }
            val port = etBrokerPort.text.toString().trim().toIntOrNull() ?: Prefs.brokerPort
            val mUser = etMqttUser.text.toString().ifEmpty { Prefs.mqttUser }
            val mPass = etMqttPass.text.toString().ifEmpty { Prefs.mqttPass }
            val json = org.json.JSONObject()
                .put("type", "wifi_cfg")
                .put("ssid", ssid)
                .put("pass", etWifiPass.text.toString())
                .put("host", host)
                .put("port", port)
            if (mUser.isNotEmpty()) {
                json.put("mqtt_user", mUser).put("mqtt_pass", mPass)
            }
            startService(
                Intent(this, com.gagaai.app.service.GagaService::class.java)
                    .setAction(com.gagaai.app.service.GagaService.ACTION_SEND_WIFI)
                    .putExtra("json", json.toString()))
            Toast.makeText(this, "已下发到嘎嘎 · 蓝牙断 60s 后自动切家模式", Toast.LENGTH_LONG).show()
        }

        val sw = view.findViewById<Switch>(R.id.swTerminal)
        sw.isChecked = Prefs.showTerminal
        sw.setOnCheckedChangeListener { _, checked ->
            Prefs.setShowTerminal(this, checked)
            applyTerminalVisibility()
        }
        // 设备名（ADR-064）：Set → 经 GagaService 组 set_name 本地信令下发到
        // 设备 NVS。名字的权威存储在设备端，App 不存（哑管道纪律）
        val etName = view.findViewById<EditText>(R.id.etDevName)
        view.findViewById<Button>(R.id.btnSendName).setOnClickListener {
            val name = etName.text.toString().trim()
            if (name.isEmpty()) {
                Toast.makeText(this, "名字不能为空", Toast.LENGTH_SHORT).show()
                return@setOnClickListener
            }
            startService(
                Intent(this, com.gagaai.app.service.GagaService::class.java)
                    .setAction(com.gagaai.app.service.GagaService.ACTION_SEND_NAME)
                    .putExtra("name", name))
            Toast.makeText(this, "已发送到嘎嘎，下一条消息生效", Toast.LENGTH_SHORT).show()
        }
        // 电池白名单状态实时显示（打开对话框的一瞬读一次）
        val whitelisted =
            getSystemService(PowerManager::class.java).isIgnoringBatteryOptimizations(packageName)
        view.findViewById<TextView>(R.id.tvBatteryState).text =
            if (whitelisted) "On ✓ — Doze & background limits waived"
            else "Off — tap to allow"
        // 与主界面"电池白名单"行同一路径：一键弹系统豁免框
        view.findViewById<View>(R.id.rowBatteryWhitelist).setOnClickListener {
            requestBatteryWhitelist()
        }
        // 自启动管理：ColorOS 不暴露开关状态，只给直达入口 + 手动核对文案
        view.findViewById<View>(R.id.rowAutoStart).setOnClickListener {
            KeepAlivePermission.openAutoStart(this)
        }
        view.findViewById<TextView>(R.id.tvSystemSettings).setOnClickListener {
            try {
                startActivity(
                    Intent(
                        Settings.ACTION_APPLICATION_DETAILS_SETTINGS,
                        Uri.parse("package:$packageName"),
                    )
                )
            } catch (_: Exception) {
            }
        }
        AlertDialog.Builder(this)
            .setTitle("Settings")
            .setView(view)
            .setPositiveButton("Done", null)
            .show()
    }

    /** 电池白名单一键豁免（主界面行 + 设置对话框共用）。 */
    private fun requestBatteryWhitelist() {
        val pm = getSystemService(PowerManager::class.java)
        if (pm.isIgnoringBatteryOptimizations(packageName)) {
            Toast.makeText(this, "Already whitelisted", Toast.LENGTH_SHORT).show()
            return
        }
        try {
            startActivity(
                Intent(
                    Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS,
                    Uri.parse("package:$packageName"),
                )
            )
        } catch (_: Exception) {
            // 部分 ROM 屏蔽该动作：退回系统电池优化列表页
            try {
                startActivity(Intent(Settings.ACTION_IGNORE_BATTERY_OPTIMIZATION_SETTINGS))
            } catch (_: Exception) {
                Toast.makeText(this, "Battery settings could not be opened — please set it to unrestricted manually", Toast.LENGTH_LONG).show()
            }
        }
    }

    override fun onResume() {
        super.onResume()
        BridgeState.addListener(this)
        renderPairStatus()
        // 回前台踢一脚重连：后台扫描被 ColorOS 限流是"卡在扫描中需手动
        // Stop/Start"的根因（2026-09-26）。服务在跑则重建扫描会话，
        // 没跑则顺便把它拉起
        startService(
            Intent(this, com.gagaai.app.service.GagaService::class.java).setAction(
                com.gagaai.app.companion.GagaCompanionService.ACTION_KICK_RECONNECT
            )
        )
        val whitelisted =
            getSystemService(PowerManager::class.java).isIgnoringBatteryOptimizations(packageName)
        batteryState.text = if (whitelisted) "On ✓" else "Off ›"
        batteryState.setTextColor(Color.parseColor(if (whitelisted) "#34A853" else "#E8A317"))
    }

    override fun onPause() {
        BridgeState.removeListener(this)
        super.onPause()
    }

    override fun onStateChanged(state: BridgeState.Snapshot) {
        render(state)
    }

    private fun render(state: BridgeState.Snapshot) {
        lastState = state
        val bleMark = UiStyle.mark(state.bleStatus)
        val mqttMark = UiStyle.mark(state.mqttStatus)
        val svcMark = if (state.serviceRunning) "✅" else "❌"

        tvServiceStatus.text = if (state.serviceRunning) "Running" else "Stopped"
        tvBleStatus.text = UiStyle.detail(state.bleStatus)
        tvMqttStatus.text = UiStyle.detail(state.mqttStatus)
        paintNode(nodeService, svcMark)
        paintNode(nodeBle, bleMark)
        paintNode(nodeMqtt, mqttMark)
        badgeService.visibility = if (state.serviceRunning) View.VISIBLE else View.GONE
        badgeBle.visibility = if (bleMark == "✅") View.VISIBLE else View.GONE
        badgeMqtt.visibility = if (mqttMark == "✅") View.VISIBLE else View.GONE

        // 整体状态徽章：胶囊底色随状态变色，点击出修复指引
        val allOk = svcMark == "✅" && bleMark == "✅" && mqttMark == "✅"
        val (bg, fg) = when {
            allOk -> "#E6F6EA" to "#1E7B3C"
            marksHaveIssue(svcMark, bleMark, mqttMark) -> "#FDEAE7" to "#B3402E"
            else -> "#FFF4D6" to "#9A6B00"
        }
        tvOverall.text = UiStyle.overallLine(listOf(svcMark, bleMark, mqttMark))
        tvOverall.setTextColor(Color.parseColor(fg))
        tvOverall.background = GradientDrawable().apply {
            shape = GradientDrawable.RECTANGLE
            cornerRadius = dp(14).toFloat()
            setColor(Color.parseColor(bg))
        }
        tvOverall.setPadding(dp(10), dp(4), dp(10), dp(4))

        // 服务器连接状态行（✅ 只显地址，异常时红色提示）
        serverStatusDot.text = mqttMark
        serverStatusText.text = when (mqttMark) {
            "✅" -> "Connected to ${UiStyle.detail(state.mqttStatus)}"
            "⏳" -> "Connecting to server…"
            else -> "Not connected — tap Edit to check"
        }
        serverStatusText.setTextColor(
            Color.parseColor(
                when (mqttMark) {
                    "✅" -> "#33302E"
                    "⏳" -> "#9A6B00"
                    else -> "#B3402E"
                }
            )
        )

        renderLogs(state.logs)
        if (state.serviceRunning) {
            btnToggleService.text = "⏹ Stop"
            btnToggleService.setBackgroundResource(R.drawable.bg_btn_stop)
            btnToggleService.setTextColor(Color.WHITE)
        } else {
            btnToggleService.text = "🚀 Start"
            btnToggleService.setBackgroundResource(R.drawable.bg_btn_start)
            btnToggleService.setTextColor(Color.parseColor("#33302E"))
        }
        danceDuck(state.serviceRunning)
    }

    private fun marksHaveIssue(vararg marks: String): Boolean = marks.any { it == "❌" }

    /** 整体状态徽章点击：列出问题并给对应的一键处理 */
    private fun showFixGuide() {
        val state = lastState ?: return
        val issues = mutableListOf<String>()
        if (!state.serviceRunning) issues.add("• Bridge service not running")
        if (UiStyle.mark(state.bleStatus) == "❌") issues.add("• BLE not connected: make sure GaGa is powered on and nearby")
        if (UiStyle.mark(state.mqttStatus) == "❌") issues.add("• MQTT not connected: check server address and current network")
        if (issues.isEmpty()) {
            Toast.makeText(this, "All good — GaGa is online!", Toast.LENGTH_SHORT).show()
            return
        }
        val dialog = AlertDialog.Builder(this)
            .setTitle("GaGa needs help")
            .setMessage(issues.joinToString("\n"))
        if (!state.serviceRunning) {
            dialog.setPositiveButton("Start service") { _, _ -> startBridgeService() }
        } else if (UiStyle.mark(state.mqttStatus) == "❌") {
            dialog.setPositiveButton("Check server") { _, _ -> showSettingsDialog() }
        } else {
            dialog.setPositiveButton("Got it", null)
        }
        dialog.setNegativeButton("Close", null).show()
    }

    /** 状态节点的圆环配色：✅ 绿 / ⏳ 黄 / 其余灰 */
    private fun paintNode(node: View, mark: String) {
        node.background = GradientDrawable().apply {
            shape = GradientDrawable.OVAL
            setColor(Color.WHITE)
            setStroke(
                dp(4),
                when (mark) {
                    "✅" -> Color.parseColor("#34A853")
                    "⏳" -> Color.parseColor("#F5A623")
                    else -> Color.parseColor("#D8D3C4")
                },
            )
        }
    }

    private fun dp(v: Int): Int = (v * resources.displayMetrics.density).toInt()

    /** 服务运行时鸭子左右摇摆（轴在脚底），停止时立正站好 */
    private fun danceDuck(running: Boolean) {
        if (running) {
            if (duckDance?.isRunning == true) return
            imgDuck.post {
                imgDuck.pivotX = imgDuck.width / 2f
                imgDuck.pivotY = imgDuck.height * 0.92f
                duckDance = ObjectAnimator.ofFloat(imgDuck, View.ROTATION, -7f, 7f).apply {
                    duration = 520
                    repeatCount = ObjectAnimator.INFINITE
                    repeatMode = ObjectAnimator.REVERSE
                    start()
                }
            }
        } else {
            duckDance?.cancel()
            duckDance = null
            imgDuck.rotation = 0f
        }
    }

    private fun renderLogs(logs: List<String>) {
        val lastLine = logs.lastOrNull()
        if (logs.size == renderedLogCount && lastLine == renderedLastLine) return
        // 增量追加条件：已渲染部分是 logs 的前缀（最后一行能对上）。
        // 缓冲回卷（条数不变但内容位移）或清空时全量重渲染。
        val prefixIntact = renderedLogCount in 1 until logs.size &&
            logs[renderedLogCount - 1] == renderedLastLine
        // 追尾模式：用户本来就贴着底部才自动吸底；正在往回翻阅时新日志不打扰。
        // 首次渲染（renderedLogCount < 0）直接吸底，保证进来看到的是最新日志。
        val stickToBottom = renderedLogCount < 0 || !logScroll.canScrollVertically(1)
        if (prefixIntact) {
            val appended = logs.subList(renderedLogCount, logs.size)
            tvLog.append(appended.joinToString("\n", prefix = "\n"))
        } else {
            tvLog.text = logs.joinToString("\n")
        }
        renderedLogCount = logs.size
        renderedLastLine = lastLine
        if (stickToBottom) {
            // 布局完成后吸底。必须用 scrollTo 而不是 fullScroll：
            // fullScroll 会把焦点塞给子 View，焦点变化冒泡到外层页面跟着滚，日志就上下横跳。
            logScroll.post {
                val child = logScroll.getChildAt(0)
                logScroll.scrollTo(0, maxOf(0, (child?.height ?: 0) - logScroll.height))
            }
        }
    }

    // region 自拉服务

    private fun startBridgeService() {
        try {
            startForegroundService(GagaService.startIntent(this))
        } catch (e: Exception) {
            Toast.makeText(this, "Service start failed: ${e.message}", Toast.LENGTH_SHORT).show()
        }
    }

    // endregion

    // region 伴生设备（CDM，ADR-025）

    private fun companionManager(): CompanionDeviceManager? =
        getSystemService(CompanionDeviceManager::class.java)

    private fun startCompanionPairing() {
        val cdm = companionManager()
        if (cdm == null) {
            Toast.makeText(this, "Companion devices not supported here", Toast.LENGTH_SHORT).show()
            return
        }
        // 嘎嘎连着 BLE 时不广播，CDM 选择框扫不到它——自动断开让它恢复广播，
        // 配对结束（成功/取消/失败）都自动把服务拉回来，不需要用户手动倒腾
        pairingPausedService = BridgeState.current().serviceRunning
        if (pairingPausedService) {
            BridgeState.log("Disconnecting BLE so GaGa advertises again — reconnecting after pairing")
            // 先撤看门狗：15 分钟闹钟若在配对窗口到点会把服务拉回来，BLE 又连上、嘎嘎又不广播了
            KeepAlive.cancelWatchdog(this)
            try {
                stopService(Intent(this, GagaService::class.java))
            } catch (_: Exception) {
            }
            // 等系统真正断开 GATT、嘎嘎恢复广播，再弹配对框
            window?.decorView?.postDelayed({ requestAssociation(cdm) }, 1500)
            return
        }
        requestAssociation(cdm)
    }

    private fun requestAssociation(cdm: CompanionDeviceManager) {
        // 必须带 BLE 过滤器：空 filter 的选择框只列系统已配对的经典蓝牙设备、
        // 不做实时扫描，嘎嘎（未配对的 BLE 设备）永远不会出现；带前缀过滤后
        // 选择框才进入扫描模式，正在广播的 GAGA- 才会被发现。
        // API 33+ 声明手表 profile：系统按手表类配套 App 对待——后台运行/
        // 后台起前台服务的豁免直接授予（保命等级质变），manifest 需同时声明
        // REQUEST_COMPANION_PROFILE_WATCH 权限
        val builder = AssociationRequest.Builder()
            .addDeviceFilter(
                android.companion.BluetoothDeviceFilter.Builder()
                    .setNamePattern(java.util.regex.Pattern.compile("GAGA-.*"))
                    .build()
            )
        if (Build.VERSION.SDK_INT >= 33) {
            builder.setDeviceProfile(AssociationRequest.DEVICE_PROFILE_WATCH)
        }
        val request = builder.build()
        try {
            cdm.associate(request, object : CompanionDeviceManager.Callback() {
                override fun onDeviceFound(chooserIntent: IntentSender) {
                    // 系统设备选择框：用户在列表里点嘎嘎
                    try {
                        startIntentSenderForResult(chooserIntent, REQUEST_PAIR, null, 0, 0, 0)
                    } catch (e: Exception) {
                        Toast.makeText(this@MainActivity, "Failed to open pairing dialog: ${e.message}", Toast.LENGTH_LONG).show()
                        resumeAfterPairing()
                    }
                }

                override fun onFailure(error: CharSequence?) {
                    runOnUiThread {
                        Toast.makeText(this@MainActivity, "Pairing failed: $error", Toast.LENGTH_LONG).show()
                        resumeAfterPairing()
                    }
                }
            }, null)
        } catch (e: Exception) {
            Toast.makeText(this, "Pairing request failed: ${e.message}", Toast.LENGTH_LONG).show()
            resumeAfterPairing()
        }
    }

    /** 配对流程结束（无论成败）：如果配对前服务在跑，把它拉回来 */
    private fun resumeAfterPairing() {
        if (!pairingPausedService) return
        pairingPausedService = false
        startBridgeService()
    }

    @Deprecated("Deprecated in Java")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        if (requestCode != REQUEST_PAIR) return
        if (resultCode != RESULT_OK || data == null) {
            // 用户取消或失败：一样要恢复配对前停掉的服务
            resumeAfterPairing()
            return
        }
        val device = associatedDevice(data)
        if (device == null) {
            Toast.makeText(this, "Pairing result missing device info", Toast.LENGTH_SHORT).show()
            resumeAfterPairing()
            return
        }
        // 存 MAC（后续 BLE 直连重连免扫描）+ 让正在跑的服务立即受益（下一次重连起走直连）
        Prefs.savePairedDevice(this, device.address, device.name ?: NAME_FALLBACK)
        renderPairStatus()
        resumeAfterPairing()
        Toast.makeText(this, "Paired ${device.name ?: device.address} — system wakes the bridge when GaGa appears", Toast.LENGTH_LONG).show()
    }

    /**
     * 从 CDM 配对结果 Intent 里取设备。Android 13+ 的 EXTRA_DEVICE 装的是
     * ScanResult（此前按 BluetoothDevice 强转会直接 ClassCastException 闪退——
     * 正是"配对框里选完嘎嘎就崩"的元凶），需要从 scanResult.device 拿。
     */
    private fun associatedDevice(data: Intent): BluetoothDevice? {
        if (Build.VERSION.SDK_INT >= 33) {
            val scanResult = data.getParcelableExtra(
                CompanionDeviceManager.EXTRA_DEVICE,
                android.bluetooth.le.ScanResult::class.java,
            )
            if (scanResult != null) return scanResult.device
            return data.getParcelableExtra(
                CompanionDeviceManager.EXTRA_DEVICE,
                BluetoothDevice::class.java,
            )
        }
        @Suppress("DEPRECATION")
        return data.getParcelableExtra(CompanionDeviceManager.EXTRA_DEVICE)
    }

    private fun unpair() {
        val cdm = companionManager()
        try {
            cdm?.associations?.forEach { cdm.disassociate(it) }
        } catch (_: Exception) {
        }
        Prefs.clearPairedDevice(this)
        renderPairStatus()
        Toast.makeText(this, "Unpaired", Toast.LENGTH_SHORT).show()
    }

    private fun renderPairStatus() {
        val paired = Prefs.pairedMac.isNotEmpty()
        companionState.text = if (paired) "Paired ✓" else "Not paired ›"
        companionState.setTextColor(Color.parseColor(if (paired) "#34A853" else "#E8A317"))
        tvPairStatus.visibility = if (paired) View.VISIBLE else View.GONE
        tvPairStatus.text = "GaGa: ${Prefs.pairedName.ifEmpty { "GaGa" }} (${Prefs.pairedMac}, direct connect on)"
    }

    // endregion

    // region 权限

    private fun requiredPermissions(): Array<String> {
        val list = mutableListOf<String>()
        if (Build.VERSION.SDK_INT >= 31) {
            list.add(Manifest.permission.BLUETOOTH_SCAN)
            list.add(Manifest.permission.BLUETOOTH_CONNECT)
        }
        list.add(Manifest.permission.ACCESS_FINE_LOCATION)
        if (Build.VERSION.SDK_INT >= 33) {
            list.add(Manifest.permission.POST_NOTIFICATIONS)
        }
        return list.toTypedArray()
    }

    private fun hasRuntimePermissions(): Boolean =
        requiredPermissions().all { checkSelfPermission(it) == PackageManager.PERMISSION_GRANTED }

    private fun requestRuntimePermissions() {
        val missing = requiredPermissions()
            .filter { checkSelfPermission(it) != PackageManager.PERMISSION_GRANTED }
            .toTypedArray()
        if (missing.isNotEmpty()) {
            requestPermissions(missing, REQUEST_PERMISSIONS)
        }
    }

    override fun onRequestPermissionsResult(
        requestCode: Int,
        permissions: Array<out String>,
        grantResults: IntArray,
    ) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (requestCode == REQUEST_PERMISSIONS &&
            grantResults.any { it != PackageManager.PERMISSION_GRANTED }
        ) {
            Toast.makeText(this, "Missing permissions break BLE scan / notifications", Toast.LENGTH_LONG).show()
        }
        // WiFi 列表选择：授权成功自动重试弹列表；被"拒绝且不再询问"→ 直达系统设置
        if (requestCode == REQ_PICK_WIFI) {
            val pairs = permissions.zip(grantResults.toTypedArray())
            android.util.Log.d("gaga.wifi",
                "权限回调：" + pairs.joinToString { (p, g) -> "$p=$g" })
            val allGranted = grantResults.isNotEmpty() &&
                grantResults.all { it == PackageManager.PERMISSION_GRANTED }
            if (allGranted) {
                pendingWifiPick?.let { pickWifiSsid(it) }
            } else if (pendingWifiPick != null) {
                val blocked = permissions.any {
                    !shouldShowRequestPermissionRationale(it)
                }
                if (blocked) {
                    Toast.makeText(this, "权限被永久拒绝，带你去系统设置手动开", Toast.LENGTH_LONG).show()
                    try {
                        startActivity(Intent(
                            android.provider.Settings.ACTION_APPLICATION_DETAILS_SETTINGS,
                            Uri.parse("package:$packageName")))
                    } catch (_: Exception) {
                    }
                }
            }
            pendingWifiPick = null
        }
    }

    companion object {
        private const val REQUEST_PERMISSIONS = 1001
        private const val REQUEST_PAIR = 1002
        private const val REQ_PICK_WIFI = 1003
        private const val NAME_FALLBACK = "GAGA"
    }
}
