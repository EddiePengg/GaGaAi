package com.gagaai.app.util

/**
 * 状态显示的统一趣味化渲染（主界面 + 常驻通知共用）。
 * 输入是 BridgeState 里的内部状态串（显示层解析，ADR-019 破例精神，不影响转发）：
 * 已连接 = 绿勾 ✅，进行中 = ⏳，其余（断开/失败/停止）= ❌。
 */
object UiStyle {

    fun mark(raw: String): String = when {
        raw.startsWith("Connected ") -> "✅"
        raw.contains("…") || raw.contains("reconnect") || raw.contains("Connect") || raw.contains("Scan") ||
            raw.contains("direct") -> "⏳"
        else -> "❌"
    }

    fun detail(raw: String): String =
        if (raw.startsWith("Connected ")) raw.removePrefix("Connected ") else raw

    /** 三节点（服务/BLE/MQTT）的整体状态一句话 */
    fun overallLine(marks: List<String>): String = when {
        marks.all { it == "✅" } -> "🟢 All good — GaGa is online!"
        marks.any { it == "❌" } -> "🔴 GaGa lost connection…"
        else -> "🟡 GaGa is connecting…"
    }
}
