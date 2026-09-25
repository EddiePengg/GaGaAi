package com.gagaai.app.util

import android.os.Handler
import android.os.Looper
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import java.util.concurrent.CopyOnWriteArrayList

/**
 * 服务与 Activity 之间的轻量状态总线（同进程）。
 * 日志存环形缓冲（最近 [MAX_LOG_LINES] 条），UI 任何时候 attach 都能回放全量，
 * Activity 重建（旋转/切后台）日志不丢。所有回调都 post 到主线程。
 */
object BridgeState {

    const val MAX_LOG_LINES = 200

    data class Snapshot(
        val serviceRunning: Boolean = false,
        val bleStatus: String = "-",
        val mqttStatus: String = "-",
        val logs: List<String> = emptyList(),
    )

    interface Listener {
        fun onStateChanged(state: Snapshot)
    }

    private val mainHandler = Handler(Looper.getMainLooper())
    private val listeners = CopyOnWriteArrayList<Listener>()
    private val timeFormat = SimpleDateFormat("HH:mm:ss", Locale.US)
    private val lock = Any()

    /** 一条日志；progressKey 非空表示可被同 key 的后续进度行原地替换（音频聚合用）。 */
    private data class Entry(val time: String, val text: String, val progressKey: String? = null) {
        fun line(): String = "$time  $text"
    }

    private val logBuffer = ArrayDeque<Entry>()

    @Volatile
    private var snapshot = Snapshot()

    fun current(): Snapshot = snapshot

    fun addListener(listener: Listener) {
        listeners.add(listener)
        listener.onStateChanged(snapshot)
    }

    fun removeListener(listener: Listener) {
        listeners.remove(listener)
    }

    fun setServiceRunning(running: Boolean) {
        update { it.copy(serviceRunning = running) }
    }

    fun setBleStatus(status: String) {
        update { it.copy(bleStatus = status) }
    }

    fun setMqttStatus(status: String) {
        update { it.copy(mqttStatus = status) }
    }

    fun log(message: String) {
        val entry = Entry(timeFormat.format(Date()), message)
        val logs: List<String>
        synchronized(lock) {
            logBuffer.addLast(entry)
            while (logBuffer.size > MAX_LOG_LINES) logBuffer.removeFirst()
            logs = snapshotLines()
        }
        update { it.copy(logs = logs) }
    }

    /**
     * 进度日志：若当前最后一行是同 key 的进度行，则原地替换（保留会话起始时间戳），
     * 否则追加新行。用于"🎙 音频上行中… 已发 N 包"这类原地刷新的显示。
     */
    fun logProgress(key: String, message: String) {
        val logs: List<String>
        synchronized(lock) {
            val last = logBuffer.lastOrNull()
            if (last != null && last.progressKey == key) {
                logBuffer.removeLast()
                logBuffer.addLast(Entry(last.time, message, key))
            } else {
                logBuffer.addLast(Entry(timeFormat.format(Date()), message, key))
                while (logBuffer.size > MAX_LOG_LINES) logBuffer.removeFirst()
            }
            logs = snapshotLines()
        }
        update { it.copy(logs = logs) }
    }

    /**
     * 进度行定稿：原地替换为最终文案，并摘掉 key——
     * 下一次同 key 的 logProgress 会另起新行，而不是覆盖这条结果。
     */
    fun logProgressFinal(key: String, message: String) {
        val logs: List<String>
        synchronized(lock) {
            val last = logBuffer.lastOrNull()
            if (last != null && last.progressKey == key) {
                logBuffer.removeLast()
                logBuffer.addLast(Entry(last.time, message))
            } else {
                logBuffer.addLast(Entry(timeFormat.format(Date()), message))
                while (logBuffer.size > MAX_LOG_LINES) logBuffer.removeFirst()
            }
            logs = snapshotLines()
        }
        update { it.copy(logs = logs) }
    }

    fun clearLogs() {
        synchronized(lock) {
            logBuffer.clear()
        }
        update { it.copy(logs = emptyList()) }
    }

    private fun snapshotLines(): List<String> = logBuffer.map { it.line() }

    private fun update(transform: (Snapshot) -> Snapshot) {
        val next = transform(snapshot)
        snapshot = next
        mainHandler.post {
            for (l in listeners) l.onStateChanged(next)
        }
    }
}
