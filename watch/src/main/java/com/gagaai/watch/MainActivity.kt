package com.gagaai.watch

import android.Manifest
import android.app.Activity
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.PackageManager
import android.graphics.Color
import android.hardware.Sensor
import android.hardware.SensorManager
import android.os.BatteryManager
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.view.GestureDetector
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.View
import android.view.ViewGroup
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.SeekBar
import android.widget.ScrollView
import android.widget.TextView
import android.widget.Toast
import com.gagaai.watch.audio.OpusEncoder
import com.gagaai.watch.audio.Recorder
import com.gagaai.watch.audio.Sounds
import com.gagaai.watch.motion.ShakeDetector
import com.gagaai.watch.msg.MsgLog
import com.gagaai.watch.msg.MsgStore
import com.gagaai.watch.mqtt.MqttManager
import com.gagaai.watch.proto.Frame
import com.gagaai.watch.proto.Reassembler
import com.gagaai.watch.proto.Signaling
import android.widget.ImageView
import org.json.JSONObject
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * 手表侧主界面 v0.2.0：嘎嘎的"第二副身躯"。
 *
 * 视图（圆屏 5 层，FrameLayout 叠放 + visibility 切换）：
 *   ① 消息卡首页（默认）  ② 嘎嘎页（左滑）  ③ 快捷面板（顶部下滑）
 *   ④ 设置页（面板/齿轮）  ⑤ 调试页（面板）
 *
 * 交互要点（2026-09-27 ADR-057/058/059 定稿）：
 *   - **启动**：系统级快捷方式（紧握松开手势 / 双击右下键）唤起，起来就收声（打开即录音）
 *   - **发送**：**再紧握一次**（onNewIntent）或**晃两下手腕** = 停止并发送——
 *     不用碰物理键、不用碰屏幕
 *   - **物理键零接触**（ADR-059）：不消费不拦截不映射，系统键留给系统
 *   - **顶部下滑** = 快捷面板（设置/调试两个入口，见 ③）
 *   - **首页左滑** = 嘎嘎页（满屏鸭子 + 大字状态）
 *
 * 为什么不按住说话：胸前两段式按钮"稍微松力就断"的物理问题（2026-09-25
 * 真机反馈），手表屏小手滑，切换式最稳——ADR-045 v1 踩过的同款坑。
 */
class MainActivity : Activity() {

    // ---------- 视图 ----------
    private lateinit var viewCards: View
    private lateinit var viewDuck: View
    private lateinit var viewPanel: View
    private lateinit var viewSettings: View
    private lateinit var viewDebug: View
    private lateinit var viewDetail: View   // 消息详情（点卡片进入，v0.3.2）

    private lateinit var tvClock: TextView
    private lateinit var tvBattery: TextView
    private lateinit var tvLink: TextView
    private lateinit var tvNoBroker: TextView
    private lateinit var listCards: LinearLayout
    private lateinit var tvStatePill: TextView
    private lateinit var btnRecord: ImageView

    private lateinit var imgDuck: ImageView
    private lateinit var tvDuckState: TextView

    private lateinit var tvKeyCode: TextView
    private lateinit var tvKeyDetail: TextView
    private lateinit var tvAcc: TextView
    private lateinit var tvGyr: TextView
    private lateinit var tvShake: TextView
    private lateinit var tvLog: TextView

    private lateinit var etBroker: EditText
    private lateinit var etMqttUser: EditText
    private lateinit var etMqttPass: EditText
    private lateinit var etDeviceId: EditText

    private lateinit var tvDetailTime: TextView
    private lateinit var tvDetailAsk: TextView
    private lateinit var tvDetailReply: TextView

    // ---------- 数据 ----------
    private val prefs by lazy { getSharedPreferences("gaga_watch", MODE_PRIVATE) }
    private val msgLog by lazy { MsgLog(msgKeepCount, MsgStore(prefs)) }   // 跨启动保留（v0.2.7）
    private lateinit var sounds: Sounds
    private lateinit var shake: ShakeDetector

    private var deviceId = "watch-01"
    private var brokerHost = ""
    private var brokerPort = 1883
    private var mqttUser = ""      // MQTT 鉴权（ADR-061：公网暴露必填；空=匿名）
    private var mqttPass = ""
    private var autoRecOn = true
    private var shakeRecOn = false
    private var msgKeepCount = 50

    private val handler = Handler(Looper.getMainLooper())
    private val logLines = ArrayDeque<String>()

    // ---------- 录音状态机 ----------
    // 四态：IDLE 待机 → RECORDING 录音中 → SENDING 发送中 → WAITING 接收中 → IDLE
    // 用户点名要看到"录音中、发送中还是接收中"三态，SENDING→WAITING 按 receipt 切换
    private enum class State { IDLE, RECORDING, SENDING, WAITING }
    private var state = State.IDLE
    private var recStartAtUptimeMs = 0L
    

    private val recorder = Recorder(
        onOpusPacket = { pkt -> MqttManager.publishUplink(Frame.encode(Frame.TYPE_OPUS, pkt)) },
        onError = { msg -> runOnUiThread { showError(msg) } },
    )

    /** 对账：10s 无 receipt 起轮询（协议 docs/protocol.md §3） */
    private val pendingQuery = object : Runnable {
        override fun run() {
            if (state == State.SENDING || state == State.WAITING) {
                MqttManager.publishUplink(Frame.encodeJson(Signaling.recStatusQuery(deviceId)))
                handler.postDelayed(this, 10_000)
            }
        }
    }

    /** 时钟 / 电量 / 传感器刷新（只在需要的视图上跑，省电） */
    private val tick = object : Runnable {
        override fun run() {
            refreshClock()
            if (viewDebug.visibility == View.VISIBLE) refreshDebug()
            handler.postDelayed(this, 1000)
        }
    }

    /** 调试页传感器刷新要更密 */
    private val sensorTick = object : Runnable {
        override fun run() {
            if (viewDebug.visibility == View.VISIBLE) {
                refreshDebug()
                handler.postDelayed(this, 100)
            }
        }
    }

    private var gesture: GestureDetector? = null

    // ---------- 生命周期 ----------

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)
        // 常亮双保险：布局里已有 keepScreenOn，再挂 Window 级 flag——灭屏会
        // 掐掉手势传感器和（ColorOS 的）后台网络，录音交互整个失效
        window.addFlags(android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        bindViews()
        loadPrefs()
        setupGestures()
        setupSounds()
        setupSensors()
        setupMqtt()
        setupButtons()

        if (!OpusEncoder.isSupported()) {
            tvStatePill.text = getString(R.string.opus_unsupported)
        }

        ensureMicPermission()
        showView(viewCards)
        renderCards()       // 恢复历史消息卡（v0.2.7：跨启动保留）
        sounds.quack()          // 开机"嘎"（用户定稿）
        log("App 启动")

        if (autoRecOn) handler.postDelayed({ if (state == State.IDLE) toggleRecord() }, 500)
        handler.post(tick)
    }

    override fun onResume() {
        super.onResume()
        if (shakeRecOn || viewDebug.visibility == View.VISIBLE) shake.start()
    }

    /**
     * 系统手势（紧握松开 / 双击右下键）唤起**已在前台**的 App 时走这里
     *（launchMode=singleTask，不重建实例）——借系统的手势识别当"切换录音"：
     * 紧握 #1 冷启动 → onCreate 自动开录；紧握 #2 前台 → onNewIntent 停止发送；
     * 紧握 #3 → 再开录（新一轮）。零按键零触屏零自研识别（ADR-058）。
     */
    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        setIntent(intent)
        toggleRecord()
    }

    override fun onPause() {
        super.onPause()
        shake.stop()   // 息屏绝不能让 sensor 持续唤醒 CPU
        // 灭屏/切后台兜底（ADR-060）：正在录音就把已说的话发出去——
        // 灭屏后手势没了、网可能被 ColorOS 掐，挂着只会烂在缓冲里
        if (state == State.RECORDING) {
            log("灭屏时仍在录音，自动收尾发送")
            stopAndSend()
        }
    }

    override fun onDestroy() {
        super.onDestroy()
        handler.removeCallbacksAndMessages(null)
        recorder.stop()
        MqttManager.stop()
    }

    // ---------- 装配 ----------

    private fun bindViews() {
        viewCards = findViewById(R.id.viewCards)
        viewDuck = findViewById(R.id.viewDuck)
        viewPanel = findViewById(R.id.viewPanel)
        viewSettings = findViewById(R.id.viewSettings)
        viewDebug = findViewById(R.id.viewDebug)
        viewDetail = findViewById(R.id.viewDetail)

        tvDetailTime = findViewById(R.id.tvDetailTime)
        tvDetailAsk = findViewById(R.id.tvDetailAsk)
        tvDetailReply = findViewById(R.id.tvDetailReply)

        tvClock = findViewById(R.id.tvClock)
        tvBattery = findViewById(R.id.tvBattery)
        tvLink = findViewById(R.id.tvLink)
        tvNoBroker = findViewById(R.id.tvNoBroker)
        listCards = findViewById(R.id.listCards)
        tvStatePill = findViewById(R.id.tvStatePill)
        btnRecord = findViewById(R.id.btnRecord)

        imgDuck = findViewById(R.id.imgDuck)
        tvDuckState = findViewById(R.id.tvDuckState)

        tvKeyCode = findViewById(R.id.tvKeyCode)
        tvKeyDetail = findViewById(R.id.tvKeyDetail)
        tvAcc = findViewById(R.id.tvAcc)
        tvGyr = findViewById(R.id.tvGyr)
        tvShake = findViewById(R.id.tvShake)
        tvLog = findViewById(R.id.tvLog)

        etBroker = findViewById(R.id.etBroker)
        etMqttUser = findViewById(R.id.etMqttUser)
        etMqttPass = findViewById(R.id.etMqttPass)
        etDeviceId = findViewById(R.id.etDeviceId)
    }

    private fun loadPrefs() {
        deviceId = prefs.getString("device_id", Build.MODEL?.replace(' ', '-')?.lowercase() ?: "watch-01") ?: "watch-01"
        brokerHost = prefs.getString("broker_host", "") ?: ""
        brokerPort = prefs.getInt("broker_port", 1883)
        mqttUser = prefs.getString("mqtt_user", "") ?: ""
        mqttPass = prefs.getString("mqtt_pass", "") ?: ""
        autoRecOn = prefs.getBoolean("auto_rec", true)
        // 双晃录音是主交互（ADR-057）：默认开。老用户显式关过（键存在）则尊重。
        shakeRecOn = prefs.getBoolean("shake_rec", true)
        soundsEnabled = prefs.getBoolean("sound_on", true)
        msgKeepCount = prefs.getInt("msg_keep", 50)
    }

    private var soundsEnabled = true

    private fun setupSounds() {
        sounds = Sounds(this)
        sounds.enabled = soundsEnabled
    }

    private fun setupSensors() {
        shake = ShakeDetector(this) { runOnUiThread { onShake() } }
        shake.threshold = prefs.getFloat("shake_threshold", 3.0f)
    }

    /** 下行分片重组器：服务端下行按 512B/包分片，>509B 的信令必须重组（v0.2.6） */
    private val downlinkFrames = Reassembler()

    /** 手势：左滑=嘎嘎页，右滑=回首页，顶部下滑=快捷面板 */
    private fun setupGestures() {
        gesture = GestureDetector(this, object : GestureDetector.SimpleOnGestureListener() {
            private val flingMin = 120f

            override fun onDown(e: MotionEvent) = true

            override fun onFling(e1: MotionEvent?, e2: MotionEvent, vx: Float, vy: Float): Boolean {
                if (e1 == null) return false
                val dx = e2.x - e1.x
                val dy = e2.y - e1.y
                if (kotlin.math.abs(dx) > kotlin.math.abs(dy)) {
                    if (kotlin.math.abs(vx) < flingMin) return false
                    if (dx < 0) { showView(viewDuck); return true }
                    // 从左往右滑 = 返回上一层（手表标准交互，v0.3.2）：
                    // 嘎嘎页/面板/设置/调试/详情，任何非首页视图都回首页
                    if (viewCards.visibility != View.VISIBLE) { showView(viewCards); return true }
                } else {
                    if (kotlin.math.abs(vy) < flingMin) return false
                    if (dy > 0 && e1.y < 140f) { showView(viewPanel); return true }   // 顶部下滑
                    if (dy < 0 && viewPanel.visibility == View.VISIBLE) { showView(viewCards); return true }
                }
                return false
            }

            override fun onSingleTapConfirmed(e: MotionEvent): Boolean {
                // 嘎嘎页点鸭子 = 切换录音（跟按键同权）
                if (viewDuck.visibility == View.VISIBLE) {
                    toggleRecord()
                    return true
                }
                return false
            }
        })
    }

    private fun setupButtons() {
        btnRecord.setOnClickListener { toggleRecord() }
        tvStatePill.setOnClickListener { toggleRecord() }
        tvNoBroker.setOnClickListener { showView(viewSettings) }

        findViewById<View>(R.id.btnOpenSettings).setOnClickListener {
            fillSettingsForm()
            showView(viewSettings)
        }
        findViewById<View>(R.id.btnOpenDebug).setOnClickListener { showView(viewDebug) }
        findViewById<View>(R.id.btnClosePanel).setOnClickListener { showView(viewCards) }
        findViewById<View>(R.id.btnToggleSound).setOnClickListener {
            soundsEnabled = !soundsEnabled
            sounds.enabled = soundsEnabled
            prefs.edit().putBoolean("sound_on", soundsEnabled).apply()
            updateSoundToggleText()
        }

        findViewById<View>(R.id.btnSave).setOnClickListener { saveSettings() }
        findViewById<View>(R.id.btnTestConn).setOnClickListener { testConnection() }
        findViewById<View>(R.id.btnClearAll).setOnClickListener {
            prefs.edit().clear().apply()
            loadPrefs()
            fillSettingsForm()
            toast("配置已清除")
        }

        findViewById<View>(R.id.btnSndQuack).setOnClickListener { sounds.quack() }
        findViewById<View>(R.id.btnSndChime).setOnClickListener { sounds.chime() }
        findViewById<View>(R.id.btnSndPoot).setOnClickListener { sounds.poot() }

        findViewById<SeekBar>(R.id.seekThreshold).setOnSeekBarChangeListener(
            object : SeekBar.OnSeekBarChangeListener {
                override fun onProgressChanged(sb: SeekBar, p: Int, fromUser: Boolean) {
                    if (!fromUser) return
                    shake.threshold = 0.5f + p / 10f        // 0.5 ~ 10.5
                    prefs.edit().putFloat("shake_threshold", shake.threshold).apply()
                }
                override fun onStartTrackingTouch(sb: SeekBar) {}
                override fun onStopTrackingTouch(sb: SeekBar) {}
            }
        )
    }

    // ---------- 视图切换 ----------

    private fun showView(v: View) {
        val wasDebug = viewDebug.visibility == View.VISIBLE
        listOf(viewCards, viewDuck, viewPanel, viewSettings, viewDebug, viewDetail).forEach {
            it.visibility = if (it === v) View.VISIBLE else View.GONE
        }
        if (v === viewSettings) fillSettingsForm()
        if (v === viewDebug) {
            // 调试页无条件跑传感器：用户进来看的就是 ACC/GYR 读不读得到
            //（v0.2.5 bug：只有"晃动录音"开关开着才 start，默认关 → 全 0，
            // 用户以为手表没有加速度计/陀螺仪）
            shake.start()
            handler.removeCallbacks(sensorTick)
            handler.post(sensorTick)
        } else if (wasDebug && !shakeRecOn) {
            shake.stop()   // 离开调试页且没开晃动录音：停传感器省电
        }
        updateStateUi()
    }

    override fun dispatchTouchEvent(ev: MotionEvent): Boolean {
        gesture?.onTouchEvent(ev)
        return super.dispatchTouchEvent(ev)
    }

    // ---------- 录音状态机 ----------

    private fun toggleRecord() {
        when (state) {
            State.IDLE -> startRecording()
            State.RECORDING -> stopAndSend()
            State.SENDING, State.WAITING -> {
                // 发送中/接收中再按：取消等待，直接回到可说话状态
                handler.removeCallbacks(pendingQuery)
                state = State.IDLE
                updateStateUi()
            }
        }
    }

    private fun startRecording() {
        if (!hasMicPermission()) { requestMicPermission(); return }
        if (brokerHost.isBlank()) {
            toast("未配置服务器")
            showView(viewSettings)
            return
        }
        state = State.RECORDING
        msgLog.addPending()
        renderCards()
        recStartAtUptimeMs = SystemClock.uptimeMillis()
        // rec_start 先于音频帧：服务端据此开流式 ASR（session.py）
        MqttManager.publishUplink(Frame.encodeJson(Signaling.recStart(deviceId)))
        recorder.start()
        sounds.quack()          // 开录"嘎"（用户定稿）
        log("rec_start")
        updateStateUi()
    }

    private fun stopAndSend() {
        val dur = (SystemClock.uptimeMillis() - recStartAtUptimeMs).toInt()
        recorder.stop()
        MqttManager.publishUplink(Frame.encodeJson(Signaling.recStop(dur, deviceId)))
        state = State.SENDING
        sounds.quack()          // 说完"嘎"（用户定稿）
        log("rec_stop ${dur}ms")
        updateStateUi()
        handler.removeCallbacks(pendingQuery)
        handler.postDelayed(pendingQuery, 10_000)
    }

    /** 双晃两下手腕 = 切换录音（ADR-057 主交互：说完了不用按键不用碰屏幕） */
    private fun onShake() {
        if (!shakeRecOn) return
        toggleRecord()
    }

    private fun updateStateUi() {
        val (txt, color) = when (state) {
            State.IDLE -> getString(R.string.state_idle) to Color.parseColor("#FFC93C")
            State.RECORDING -> getString(R.string.state_recording) to Color.parseColor("#E8503A")
            State.SENDING -> getString(R.string.state_sending) to Color.parseColor("#F5A623")
            State.WAITING -> getString(R.string.state_waiting) to Color.parseColor("#34A853")
        }
        tvStatePill.text = txt
        tvDuckState.text = txt
        // 状态染色鸭子（真图，不再自绘）：待机原色，录音红、发送橙、接收绿
        imgDuck.colorFilter = when (state) {
            State.IDLE -> null
            State.RECORDING -> android.graphics.PorterDuffColorFilter(
                Color.parseColor("#66E8503A"), android.graphics.PorterDuff.Mode.SRC_ATOP
            )
            State.SENDING -> android.graphics.PorterDuffColorFilter(
                Color.parseColor("#66F5A623"), android.graphics.PorterDuff.Mode.SRC_ATOP
            )
            State.WAITING -> android.graphics.PorterDuffColorFilter(
                Color.parseColor("#6634A853"), android.graphics.PorterDuff.Mode.SRC_ATOP
            )
        }
        // 录音按钮同步染色，两处状态一眼对齐
        btnRecord.setBackgroundResource(
            if (state == State.RECORDING) R.drawable.bg_rec_button_active
            else R.drawable.bg_rec_button
        )
        // MQTT 状态用文字，不用圆点（2026-09-26 真机报障：一个圆点看不出啥意思）。
        // 连不上时把配置的地址也带出来——用户一眼能分清"配置写错了"还是"网络不通"。
        if (MqttConnected) {
            tvLink.text = "MQTT ✓"
            tvLink.setTextColor(Color.parseColor("#34A853"))
        } else if (brokerHost.isBlank()) {
            tvLink.text = "MQTT ·"
            tvLink.setTextColor(Color.parseColor("#8A8577"))
        } else {
            tvLink.text = "MQTT ✗ $brokerHost:$brokerPort"
            tvLink.setTextColor(Color.parseColor("#E8503A"))
        }
        tvNoBroker.visibility = if (brokerHost.isBlank()) View.VISIBLE else View.GONE
    }

    @Volatile
    private var MqttConnected = false

    // ---------- 信令 ----------

    private fun setupMqtt() {
        MqttManager.clientId = "watch-$deviceId"
        MqttManager.onLog = { msg -> runOnUiThread { log(msg) } }
        MqttManager.onStateChanged = { ok ->
            runOnUiThread {
                MqttConnected = ok
                log(if (ok) "MQTT 已连接" else "MQTT 断开")
                if (ok) sendHello()
                updateStateUi()
            }
        }
        MqttManager.onDownlink = { bytes ->
            // 下行载荷带 3 字节帧头（v0.2.1 修复）且 >509B 会分片（v0.2.6 修复：
            // v0.2.5 只剥头不重组，reply 中文几百字必分片 = 永远收不到，且零报错）
            for ((type, payload) in downlinkFrames.feed(bytes)) {
                if (type != Frame.TYPE_JSON) continue   // 下行音频帧（talk 模式）暂不处理
                val json = Signaling.parse(payload)
                if (json != null) runOnUiThread { handleSignaling(json) }
            }
        }
        if (brokerHost.isNotBlank()) MqttManager.start(brokerHost, brokerPort, mqttUser, mqttPass)
    }

    private fun sendHello() {
        MqttManager.publishUplink(Frame.encodeJson(Signaling.hello("gaga-$deviceId", appVersion())))
    }

    private fun handleSignaling(obj: JSONObject) {
        // 设备过滤（ADR-057 多设备串扰修复）：receipt/reply/error 带的
        // device 戳不是本机 → 忽略（胸前嘎嘎与手表同群同 broker）
        val t0 = obj.optString("type")
        val dv = obj.optString("device", "")
        if ((t0 == "receipt" || t0 == "reply" || t0 == "error") &&
            dv.isNotEmpty() && dv != deviceId) {
            log("信令属设备 $dv（非本机 $deviceId），忽略")
            return
        }
        when (t0) {
            "hello_ack" -> log("hello_ack")
            "receipt" -> {
                handler.removeCallbacks(pendingQuery)
                val text = obj.optString("text", "")
                val mid = obj.optString("msg_id", "")
                msgLog.fillAsk(mid, text)
                if (state == State.SENDING) state = State.WAITING   // 已送达，转入"接收中"
                renderCards()
                updateStateUi()
                log("receipt: $text")
            }
            "reply" -> {
                handler.removeCallbacks(pendingQuery)
                val text = obj.optString("text", "")
                msgLog.fillReply(obj.optString("reply_to", ""), text)
                state = State.IDLE
                sounds.chime()      // 收到回复"叮咚"（=钉钉音，用户要的）
                renderCards()
                updateStateUi()
                log("reply: $text")
            }
            "error" -> {
                handler.removeCallbacks(pendingQuery)
                val msg = obj.optString("msg", obj.optString("code", "未知错误"))
                msgLog.failLatest(msg)
                state = State.IDLE
                sounds.poot()
                renderCards()
                updateStateUi()
                log("error: $msg")
            }
        }
    }

    // ---------- 卡片渲染 ----------

    /**
     * 渲染消息卡。**真正的卡片块**，不是一串带冒号的字符串
     *（2026-09-26 报障："冒号识别中，嘎嘎冒号正在回复……像乱码"）。
     *
     * 每张卡两栏：
     *   ┌──────────────────────────┐
     *   │ 你            识别中…    │  ← 标签 + 状态
     *   │ 今天天气怎么样            │  ← 正文
     *   │ ─────────────────────── │
     *   │ 嘎嘎          正在回复…  │
     *   │ 晴，25℃                 │
     *   └──────────────────────────┘
     */
    private fun renderCards() {
        listCards.removeAllViews()
        val items = msgLog.snapshot()
        for (m in items) listCards.addView(buildCard(m), cardLp())

        if (items.isEmpty()) {
            listCards.addView(TextView(this).apply {
                text = "还没有消息\n按下按钮说一句"
                setTextColor(Color.parseColor("#8A8577"))
                textSize = 11f
                gravity = android.view.Gravity.CENTER
                setPadding(0, 50, 0, 0)
            })
        }
    }

    private fun cardLp() = LinearLayout.LayoutParams(
        ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT
    ).apply { bottomMargin = 8 }

    private fun buildCard(m: MsgLog.Msg): View {
        val card = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(12, 8, 12, 8)
            setBackgroundColor(if (m.pending) Color.parseColor("#2A2830") else Color.parseColor("#23222B"))
            // 点卡片看全文（v0.3.2：列表里正文截断，详情页放开）
            setOnClickListener { openDetail(m.id) }
        }

        // —— 上栏：你 ——
        val askTitle = if (m.failed.isNotEmpty()) "发送失败" else "你"
        val askTitleColor = if (m.failed.isNotEmpty()) "#E8503A" else "#FFC93C"
        val askBody = when {
            m.failed.isNotEmpty() -> ellipsize(m.failed, MAX_ASK_CHARS)
            m.ask.isNotEmpty() -> ellipsize(m.ask, MAX_ASK_CHARS)
            else -> "识别中…"
        }
        // 右上角标说话时刻（用户 2026-09-27 要的）；等 ASR 的状态正文
        // "识别中…"已表达，不在这儿重复
        val hhmm = SimpleDateFormat("HH:mm", Locale.US).format(Date(m.tsMs))
        card.addView(rowLabel(askTitle, askTitleColor, hhmm))
        card.addView(rowBody(askBody))

        // —— 分隔 ——
        card.addView(View(this).apply {
            setBackgroundColor(Color.parseColor("#3A3842"))
        }, LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, 1
        ).apply { topMargin = 6; bottomMargin = 6 })

        // —— 下栏：嘎嘎 ——
        if (m.failed.isEmpty()) {
            val replyBody = when {
                m.reply.isNotEmpty() -> ellipsize(m.reply, MAX_REPLY_CHARS)
                m.pending -> "正在回复…"
                else -> "（还没回复）"
            }
            val replyStatus = when {
                m.reply.isNotEmpty() -> if (m.reply.length > MAX_REPLY_CHARS) "全文›" else ""
                m.pending -> "等待中"
                else -> "超时"
            }
            card.addView(rowLabel("嘎嘎", "#34A853", replyStatus))
            card.addView(rowBody(replyBody))
        }
        return card
    }

    /** 列表卡正文截断：超长加省略号，全文进详情页看（v0.3.2） */
    private fun ellipsize(s: String, max: Int): String =
        if (s.length > max) s.take(max) + "…" else s

    /** 点卡片 → 详情页看全文（v0.3.2）。打开后右滑返回首页 */
    private fun openDetail(id: Long) {
        val m = msgLog.get(id) ?: return
        tvDetailTime.text = SimpleDateFormat("HH:mm", Locale.US).format(Date(m.tsMs))
        tvDetailAsk.text = when {
            m.failed.isNotEmpty() -> "（发送失败）${m.failed}"
            m.ask.isNotEmpty() -> m.ask
            else -> "（未识别到内容）"
        }
        tvDetailReply.text = when {
            m.reply.isNotEmpty() -> m.reply
            m.pending -> "正在回复…"
            else -> "（还没回复）"
        }
        showView(viewDetail)
    }

    /** 卡片小标题行：左标签 + 右侧文本（时间/状态），两端对齐 */
    private fun rowLabel(left: String, color: String, right: String): View {
        val row = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = android.view.Gravity.CENTER_VERTICAL
        }
        row.addView(TextView(this).apply {
            text = left
            setTextColor(Color.parseColor(color))
            textSize = 10f
            setTypeface(typeface, android.graphics.Typeface.BOLD)
        })
        if (right.isNotEmpty()) {
            row.addView(TextView(this).apply {
                text = right
                setTextColor(Color.parseColor("#8A8577"))
                textSize = 8f
                // 修复（v0.3.2）：右侧文本曾被 weight 拉伸占满剩余空间，但
                // View 内文本默认左对齐 → 时间贴在"你"字边上而不是行末。
                // LayoutParams.gravity 管不了"View 内部"的对齐，必须设给 TextView 自己
                gravity = android.view.Gravity.END
            }, LinearLayout.LayoutParams(
                0, ViewGroup.LayoutParams.WRAP_CONTENT
            ).apply { weight = 1f })
        }
        return row
    }

    /** 卡片正文行 */
    private fun rowBody(text: String): View = TextView(this).apply {
        this.text = text
        setTextColor(Color.parseColor("#FFF6E3"))
        textSize = 11f
        setPadding(0, 2, 0, 0)
    }

    // ---------- 调试页 ----------

    private fun refreshDebug() {
        tvAcc.text = String.format(
            Locale.US, "ACC  x:%+.2f y:%+.2f z:%+.2f", shake.accX, shake.accY, shake.accZ
        )
        tvGyr.text = String.format(
            Locale.US, "GYR  x:%+.2f y:%+.2f z:%+.2f", shake.gyrX, shake.gyrY, shake.gyrZ
        )
        val ratio = (shake.lastDelta / shake.threshold).coerceIn(0f, 1f)
        val bar = "▓".repeat((ratio * 5).toInt()) + "░".repeat(5 - (ratio * 5).toInt())
        val elapsed = System.currentTimeMillis() - shake.pendingSinceMs
        val dbl = if (shake.awaitingSecond && elapsed <= ShakeDetector.DOUBLE_WINDOW_MS) {
            val remain = (ShakeDetector.DOUBLE_WINDOW_MS - elapsed) / 1000.0
            String.format(
                Locale.US, "等${shake.pendingAxis}${if (shake.pendingSign) "+" else "-"}反向 %.1fs", remain
            )
        } else {
            "待第1晃"
        }
        tvShake.text = String.format(
            Locale.US, "晃动 %s %.2f/%.1f  %s  [%s|%s]",
            bar, shake.lastDelta, shake.threshold, dbl,
            if (shake.accAvailable) "ACC✓" else "ACC✗",
            if (shake.gyrAvailable) "GYR✓" else "GYR✗",
        )
        tvLog.text = logLines.joinToString("\n")
    }

    private fun log(msg: String) {
        val ts = SimpleDateFormat("HH:mm:ss", Locale.US).format(Date())
        logLines.addFirst("$ts  $msg")
        while (logLines.size > 60) logLines.removeLast()
        // 同步打一份到 logcat：真机排障只能靠 adb logcat，只写 UI 等于黑盒
        android.util.Log.i(TAG, msg)
        if (viewDebug.visibility == View.VISIBLE) tvLog.text = logLines.joinToString("\n")
    }

    // ---------- 设置 ----------

    private fun fillSettingsForm() {
        // **始终显示 host:port**：早期版本在端口=1883 时只显示 host，
        // 用户以为端口丢了（2026-09-26 真机报障）。端口永远显式带出来。
        etBroker.setText("$brokerHost:$brokerPort")
        etMqttUser.setText(mqttUser)
        etMqttPass.setText(mqttPass)
        etDeviceId.setText(deviceId)
        refreshLocalIps()
        findViewById<TextView>(R.id.btnSoundToggle).text =
            "提示音        [ ${if (soundsEnabled) "开" else "关"} ]"
        findViewById<TextView>(R.id.btnAutoRec).text =
            "打开即录音    [ ${if (autoRecOn) "开" else "关"} ]"
        findViewById<TextView>(R.id.btnShakeRec).text =
            "晃两下手腕    [ ${if (shakeRecOn) "开" else "关"} ]"
        findViewById<TextView>(R.id.btnKeepMsg).text =
            "消息保留      [ ${msgKeepCount} 条 ]"
        findViewById<TextView>(R.id.tvVersion).text = "版本 ${appVersion()}"

        findViewById<View>(R.id.btnSoundToggle).setOnClickListener {
            soundsEnabled = !soundsEnabled; sounds.enabled = soundsEnabled
            prefs.edit().putBoolean("sound_on", soundsEnabled).apply()
            fillSettingsForm()
        }
        findViewById<View>(R.id.btnAutoRec).setOnClickListener {
            autoRecOn = !autoRecOn
            prefs.edit().putBoolean("auto_rec", autoRecOn).apply()
            fillSettingsForm()
        }
        findViewById<View>(R.id.btnShakeRec).setOnClickListener {
            shakeRecOn = !shakeRecOn
            prefs.edit().putBoolean("shake_rec", shakeRecOn).apply()
            if (shakeRecOn) shake.start()
            else if (viewDebug.visibility != View.VISIBLE) shake.stop()   // 调试页还要看数据
            fillSettingsForm()
        }
        // 保留条数：20 → 50 → 100 → 200 循环。缩小立即截断最旧的卡并落盘
        findViewById<View>(R.id.btnKeepMsg).setOnClickListener {
            msgKeepCount = when (msgKeepCount) {
                20 -> 50; 50 -> 100; 100 -> 200; else -> 20
            }
            prefs.edit().putInt("msg_keep", msgKeepCount).apply()
            msgLog.capacity = msgKeepCount
            renderCards()
            fillSettingsForm()
        }
    }

    private fun appVersion(): String = try {
        packageManager.getPackageInfo(packageName, 0).versionName ?: "?"
    } catch (_: Exception) {
        "?"
    }

    /**
     * 解析 "host:port"。兼容中文全角冒号"："（中文输入法常出这个，
     * 早期版本按 ASCII 冒号切分导致整串被当成 host，MQTT 必连不上）。
     * 端口非法/缺省一律回落 1883。
     */
    private fun parseBroker(raw: String): Pair<String, Int> {
        val normalized = raw.replace('：', ':').trim()
        val idx = normalized.lastIndexOf(':')
        if (idx <= 0) return normalized.trim() to 1883
        val host = normalized.substring(0, idx).trim()
        val port = normalized.substring(idx + 1).trim().toIntOrNull()
        return host to (port ?: 1883)
    }

    private fun saveSettings() {
        val (h, p) = parseBroker(etBroker.text.toString())
        brokerHost = h
        brokerPort = p
        mqttUser = etMqttUser.text.toString().trim()
        mqttPass = etMqttPass.text.toString()
        deviceId = etDeviceId.text.toString().trim().ifEmpty { deviceId }
        prefs.edit()
            .putString("broker_host", brokerHost)
            .putInt("broker_port", brokerPort)
            .putString("mqtt_user", mqttUser)
            .putString("mqtt_pass", mqttPass)
            .putString("device_id", deviceId)
            .apply()
        MqttManager.stop()
        MqttManager.clientId = "watch-$deviceId"
        if (brokerHost.isNotBlank()) MqttManager.start(brokerHost, brokerPort, mqttUser, mqttPass)
        toast("已保存 $brokerHost:$brokerPort")   // 显式回显，用户能核对
        showView(viewCards)
    }

    /**
     * 网络诊断：列出本机所有网卡拿到的 IP。
     * 蓝牙 PAN 会以 `bt-pan`/`bnep` 之类的接口出现，能看到地址就说明
     * 手机的"蓝牙网络共享"确实把网分过来了（2026-09-26 用户问的就是这个）。
     */
    private fun refreshLocalIps() {
        try {
            val out = StringBuilder()
            java.util.Collections.list(java.net.NetworkInterface.getNetworkInterfaces())
                .filter { it.isUp && !it.isLoopback }
                .forEach { nif ->
                    val addrs = java.util.Collections.list(nif.inetAddresses)
                        .filterIsInstance<java.net.Inet4Address>()
                        .map { it.hostAddress }
                    if (addrs.isNotEmpty()) {
                        out.append("${nif.displayName}  ${addrs.joinToString(",")}\n")
                    }
                }
            findViewById<TextView>(R.id.tvLocalIps).text =
                if (out.isEmpty()) "本机 IP  （无可用地址）" else "本机 IP\n$out"
        } catch (t: Throwable) {
            findViewById<TextView>(R.id.tvLocalIps).text = "本机 IP  读取失败: ${t.message}"
        }
    }

    /**
     * 原始 TCP 探测 broker。不走 MQTT——先排除"网到不了"vs"MQTT 不对"两层。
     * 结果直接写在设置页，不用 adb 也能看。
     */
    private fun testConnection() {
        val (h, p) = parseBroker(etBroker.text.toString())
        val tv = findViewById<TextView>(R.id.tvTestResult)
        if (h.isBlank()) {
            tv.text = "请先填服务器地址"
            return
        }
        tv.text = "测试中 $h:$p …"
        Thread {
            var msg: String
            try {
                val t0 = System.currentTimeMillis()
                java.net.Socket().use { s ->
                    s.connect(java.net.InetSocketAddress(h, p), 4000)
                }
                val ms = System.currentTimeMillis() - t0
                msg = "✅ TCP 通  ${ms}ms\n说明网络可达，若 MQTT 还不连就是 broker/端口/防火墙问题"
            } catch (e: java.net.SocketTimeoutException) {
                msg = "❌ 超时（4s）\n网到不了 $h:$p\n→ 查手机蓝牙共享是否开启 / 手表是否拿到 IP"
            } catch (e: java.net.UnknownHostException) {
                msg = "❌ 域名解析失败\n→ 地址写错了，或 DNS 不通"
            } catch (e: java.lang.SecurityException) {
                msg = "❌ 被拒（SecurityException）\n→ INTERNET 权限缺失"
            } catch (e: Throwable) {
                msg = "❌ ${e.javaClass.simpleName}: ${e.message}\n→ 多半是网不通或端口错"
            }
            runOnUiThread {
                tv.text = msg
                log("连接测试 $h:$p → ${msg.lineSequence().first()}")
            }
        }.start()
    }

    private fun updateSoundToggleText() {
        findViewById<TextView>(R.id.btnToggleSound).text =
            "提示音：${if (soundsEnabled) "开" else "关"}"
    }

    // ---------- 硬件按键：零接触（ADR-059，2026-09-27） ----------
    //
    // 历史：OPPO Watch X3 两颗物理键实测 F1/F2（右上/右下）。v0.2.2~0.2.8
    // 期间 App 白名单拦截过它们（F2=切换录音、F1=返回），但系统冲突不断：
    // 绑定"双击 F2 打开嘎嘎"后 F1 的"回主页"在 ColorOS 策略层执行拦不住，
    // 白名单还把音量/相机等键一并纳了进去——用户按任何键都可能触发录音，
    // 观感很怪。录音交互已由紧握（ADR-058）/ 晃两下（ADR-057）/ 触屏覆盖，
    // 物理键**全部交回系统**：App 不消费、不拦截、不映射（连 MediaSession
    // 媒体键兜底也一并删除——它同样是"打按钮主意"的路径）。

    override fun dispatchKeyEvent(event: KeyEvent?): Boolean {
        if (event != null) onAnyKey(event)
        return super.dispatchKeyEvent(event)
    }

    private fun onAnyKey(event: KeyEvent) {
        // 调试页：纯观察——把到达 App 的按键记录给用户看，一个都不消费
        if (viewDebug.visibility == View.VISIBLE) {
            tvKeyCode.text = KeyEvent.keyCodeToString(event.keyCode)
            tvKeyDetail.text = "code=${event.keyCode} ${if (event.action == KeyEvent.ACTION_DOWN) "DOWN" else "UP"}" +
                " rep=${event.repeatCount}  → 交回系统"
            log("KEY ${KeyEvent.keyCodeToString(event.keyCode)} → 交回系统")
        }
    }

    // ---------- 时钟 / 电量 ----------

    private fun refreshClock() {
        tvClock.text = SimpleDateFormat("HH:mm", Locale.US).format(Date())
        tvBattery.text = "🔋 ${batteryPct()}%"
        updateStateUi()
    }

    private fun batteryPct(): Int {
        val i = registerReceiver(null, IntentFilter(Intent.ACTION_BATTERY_CHANGED)) ?: return -1
        val level = i.getIntExtra(BatteryManager.EXTRA_LEVEL, -1)
        val scale = i.getIntExtra(BatteryManager.EXTRA_SCALE, -1)
        return if (level >= 0 && scale > 0) (level * 100 / scale) else -1
    }

    // ---------- 权限 / 杂项 ----------

    private fun hasMicPermission() =
        checkSelfPermission(Manifest.permission.RECORD_AUDIO) == PackageManager.PERMISSION_GRANTED

    private fun ensureMicPermission() { if (!hasMicPermission()) requestMicPermission() }

    private fun requestMicPermission() =
        requestPermissions(arrayOf(Manifest.permission.RECORD_AUDIO), REQ_MIC)

    override fun onRequestPermissionsResult(
        requestCode: Int, permissions: Array<out String>, grantResults: IntArray,
    ) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (requestCode == REQ_MIC && (grantResults.isEmpty() || grantResults[0] != PackageManager.PERMISSION_GRANTED)) {
            toast(getString(R.string.need_mic_permission))
        }
    }

    private fun toast(msg: String) = Toast.makeText(this, msg, Toast.LENGTH_SHORT).show()

    private fun showError(msg: String) {
        state = State.IDLE
        sounds.poot()
        updateStateUi()
        log("错误: $msg")
        toast(msg)
    }

    companion object {
        private const val REQ_MIC = 1001
        /** logcat 过滤 tag：`adb logcat -s GagaWatch` */
        private const val TAG = "GagaWatch"
        /** 列表卡正文截断上限（详情页放开全文）：ask 短问、reply 长答 */
        private const val MAX_ASK_CHARS = 60
        private const val MAX_REPLY_CHARS = 80
    }
}
