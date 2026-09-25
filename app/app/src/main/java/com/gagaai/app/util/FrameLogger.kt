package com.gagaai.app.util

import android.os.Handler
import android.os.Looper
import org.json.JSONObject
import java.util.Locale

/**
 * 帧感知的日志显示层（ADR-019 哑管道原则的显示层破例）。
 *
 * 只读帧头/payload 用于"说人话"的日志展示，绝不影响转发：
 * 转发路径（BLE ↔ MQTT 字节原样透传）不经过这里。
 *
 * - 音频帧（type 0x01 / 0x81 / 续包 0x00）聚合：首包打进度行，之后节流（500ms）
 *   原地刷新计数，500ms 无新包或出现非音频包时定稿为"完成，共 N 包"。
 * - JSON 信令（type 0x02 / 0x82）解析为人性化文案；分片（0x82）标"…（分片）"，不重组。
 * - 解析失败 / 未知 type 回退为原来的字节数显示。
 * - 去重保险丝：非音频 payload 完全相同且 500ms 内重复到达只显示一次
 *   （转发链路 bug 的兜底，主修在 MqttManager 生命周期）。
 */
object FrameLogger {

    private const val TYPE_AUDIO = 0x01
    private const val TYPE_JSON = 0x02
    private const val TYPE_CONTINUATION = 0x00
    private const val FLAG_MORE_FRAGMENTS = 0x80

    private const val SETTLE_MS = 500L
    private const val REFRESH_MS = 500L

    /** 显示去重窗口：相同 payload 在这么短的时间内重复到达只显示一次（重复订阅 bug 的保险丝） */
    private const val DEDUPE_MS = 500L

    private enum class Direction(val progressKey: String, val emoji: String, val verb: String) {
        UP("audio_up", "🎙", "上行"),
        DOWN("audio_down", "🔊", "下行"),
    }

    private class AudioSession {
        var active = false
        var count = 0
        var lastRefreshAt = 0L
    }

    private val lock = Any()
    private val handler = Handler(Looper.getMainLooper())
    private val upSession = AudioSession()
    private val downSession = AudioSession()
    private val settleUp = Runnable { settle(Direction.UP) }
    private val settleDown = Runnable { settle(Direction.DOWN) }

    // 去重保险丝：每方向记录最近一条非音频 payload 及到达时间
    private val lastPayload = HashMap<Direction, Pair<ByteArray, Long>>()

    /** BLE → MQTT 的上行包（即将发布 gaga/up 的原样字节）。 */
    fun uplink(bytes: ByteArray) = handle(Direction.UP, bytes)

    /** MQTT → BLE 的下行包（gaga/down 收到的原样字节）。 */
    fun downlink(bytes: ByteArray) = handle(Direction.DOWN, bytes)

    private fun handle(dir: Direction, bytes: ByteArray) {
        if (bytes.size < 3) {
            if (isDuplicate(dir, bytes)) return
            settleAudio(dir)
            fallback(dir, bytes.size)
            return
        }
        val type = bytes[0].toInt() and 0xFF
        val baseType = type and 0x7F
        when {
            baseType == TYPE_AUDIO -> onAudio(dir)
            type == TYPE_CONTINUATION && isAudioActive(dir) -> onAudio(dir)
            baseType == TYPE_JSON -> {
                if (isDuplicate(dir, bytes)) return
                settleAudio(dir)
                onJson(dir, bytes, fragmented = type and FLAG_MORE_FRAGMENTS != 0)
            }
            else -> {
                if (isDuplicate(dir, bytes)) return
                settleAudio(dir)
                fallback(dir, bytes.size)
            }
        }
    }

    /** 相同 payload 在 [DEDUPE_MS] 内重复到达 → true（只挡显示，不影响转发）。 */
    private fun isDuplicate(dir: Direction, bytes: ByteArray): Boolean = synchronized(lock) {
        val now = System.currentTimeMillis()
        val last = lastPayload[dir]
        val dup = last != null &&
            now - last.second < DEDUPE_MS &&
            last.first.contentEquals(bytes)
        if (!dup) lastPayload[dir] = bytes.copyOf() to now
        dup
    }

    // region 音频聚合

    private fun onAudio(dir: Direction) {
        synchronized(lock) {
            val s = session(dir)
            val now = System.currentTimeMillis()
            if (!s.active) {
                s.active = true
                s.count = 1
                s.lastRefreshAt = now
                BridgeState.logProgress(dir.progressKey, progressText(dir, s.count))
            } else {
                s.count++
                if (now - s.lastRefreshAt >= REFRESH_MS) {
                    s.lastRefreshAt = now
                    BridgeState.logProgress(dir.progressKey, progressText(dir, s.count))
                }
            }
            val runnable = settleRunnable(dir)
            handler.removeCallbacks(runnable)
            handler.postDelayed(runnable, SETTLE_MS)
        }
    }

    private fun settleAudio(dir: Direction) {
        handler.post(settleRunnable(dir))
    }

    private fun settle(dir: Direction) {
        synchronized(lock) {
            val s = session(dir)
            if (!s.active) return
            s.active = false
            BridgeState.logProgressFinal(
                dir.progressKey,
                "${dir.emoji} 音频${dir.verb}完成，共 ${s.count} 包",
            )
        }
    }

    private fun progressText(dir: Direction, count: Int): String =
        "${dir.emoji} 音频${dir.verb}中… 已发 $count 包"

    private fun isAudioActive(dir: Direction): Boolean =
        synchronized(lock) { session(dir).active }

    private fun session(dir: Direction): AudioSession =
        if (dir == Direction.UP) upSession else downSession

    private fun settleRunnable(dir: Direction): Runnable =
        if (dir == Direction.UP) settleUp else settleDown

    // region JSON 信令

    private fun onJson(dir: Direction, bytes: ByteArray, fragmented: Boolean) {
        val payload = try {
            String(bytes, 3, bytes.size - 3, Charsets.UTF_8)
        } catch (_: Exception) {
            fallback(dir, bytes.size)
            return
        }
        val json = try {
            JSONObject(payload)
        } catch (_: Exception) {
            fallback(dir, bytes.size)
            return
        }
        val suffix = if (fragmented) "…（分片）" else ""
        val text: String? = when (json.optString("type")) {
            "rec_start" -> "🎙 开始录音"
            "rec_stop" -> {
                val ms = json.optLong("duration_ms", -1L)
                if (ms >= 0) {
                    "⏹ 结束录音（时长 ${String.format(Locale.US, "%.1f", ms / 1000f)}s）"
                } else {
                    "⏹ 结束录音"
                }
            }
            "hello" -> "👋 设备上线 ${json.optString("device")} fw ${json.optString("fw")}"
            "receipt" -> "✅ 叮！服务器已收到"
            "reply" -> "💬 回复：${json.optString("text")}"
            "error" -> errorText(json)
            "ping" -> {
                // 心跳用进度行原地刷新，保持单行不刷屏
                BridgeState.logProgress("ping", "💓 心跳")
                return
            }
            "talk_request" -> "🗣 请求实时对话…"
            "talk_ready" -> "🗣 对话已接通"
            "talk_asr" -> {
                streamText("talk_asr", "👂 你：${json.optString("text")}", json.optBoolean("final"), suffix)
                return
            }
            "talk_reply" -> {
                streamText("talk_reply", "🦆 鸭子：${json.optString("text")}", json.optBoolean("final"), suffix)
                return
            }
            "talk_end" -> "👋 对话结束（${talkEndReason(json.optString("reason"))}）"
            else -> null
        }
        if (text == null) {
            fallback(dir, bytes.size)
        } else {
            BridgeState.log(text + suffix)
        }
    }

    /** 流式文本信令：interim 原地刷新进度行，final 定稿。 */
    private fun streamText(key: String, text: String, final: Boolean, suffix: String) {
        if (final) {
            BridgeState.logProgressFinal(key, text + suffix)
        } else {
            BridgeState.logProgress(key, text + suffix)
        }
    }

    private fun talkEndReason(reason: String): String = when (reason) {
        "exit_intent" -> "用户说了再见"
        "device_end" -> "手动结束"
        "timeout" -> "超时"
        "timeout_max" -> "达到最大时长"
        "error" -> "出错"
        else -> reason.ifEmpty { "未知原因" }
    }

    private fun errorText(json: JSONObject): String {
        val code = json.optString("code")
        val msg = json.optString("msg")
        return when (code) {
            "BUSY" -> "❌ 忙：已在对话中"
            "REALTIME_FAIL" -> "❌ 对话建立失败：$msg"
            else -> if (code.isNotEmpty()) "❌ $code：$msg" else "❌ $msg"
        }
    }

    // region 回退

    private fun fallback(dir: Direction, size: Int) {
        if (dir == Direction.UP) {
            BridgeState.log("BLE → MQTT gaga/up ${size}B")
        } else {
            BridgeState.log("MQTT ← gaga/down ${size}B → BLE")
        }
    }
}
