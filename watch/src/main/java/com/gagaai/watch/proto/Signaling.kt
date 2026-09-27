package com.gagaai.watch.proto

import org.json.JSONObject

/**
 * JSON 信令构造 / 解析（schema 见 docs/protocol.md §3、docs/data-model.md §1）。
 * 手表只关心与"发消息 + 看回复"闭环相关的几条：
 *
 * 上行：hello / rec_start / rec_stop{duration_ms} / rec_status_query（丢回复自愈）
 * 下行：hello_ack / receipt / reply / error
 */
object Signaling {

    /** 开机/重连后自我介绍，服务端据此回 hello_ack 并对时。 */
    fun hello(device: String, fw: String): String =
        JSONObject()
            .put("type", "hello")
            .put("device", device)
            .put("fw", fw)
            .toString()

    /**
     * 开始录音。服务端收到才把会话置为 recording 并开启流式 ASR——
     * 缺了它，音频帧到了也不知道往哪投。
     */
    fun recStart(device: String): String =
        JSONObject().put("type", "rec_start").put("device", device).toString()

    /**
     * 结束录音。duration_ms 是本次实录时长，服务端据此判断"帧数是否覆盖声明时长"
     * 决定立即收尾还是走静默窗；缺了它服务端不知道该收尾。
     */
    fun recStop(durationMs: Int, device: String): String =
        JSONObject()
            .put("type", "rec_stop")
            .put("duration_ms", durationMs)
            .put("device", device)
            .toString()

    /** 对账查询：收不到 receipt/reply 时每 10s 问一次，服务端幂等重发最近结果。 */
    fun recStatusQuery(device: String): String =
        JSONObject().put("type", "rec_status_query").put("device", device).toString()

    /** 解析下行信令；非 JSON 或缺 type 字段返回 null。 */
    fun parse(payload: ByteArray): JSONObject? = try {
        val obj = JSONObject(String(payload, Charsets.UTF_8))
        if (obj.has("type")) obj else null
    } catch (_: Exception) {
        null
    }
}
