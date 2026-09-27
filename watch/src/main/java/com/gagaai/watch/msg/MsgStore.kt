package com.gagaai.watch.msg

import android.content.SharedPreferences
import org.json.JSONArray
import org.json.JSONObject

/**
 * 消息历史的 SharedPreferences JSON 落盘（v0.2.7）。
 *
 * 为什么是 SharedPreferences 而不是 SQLite/Room：几十条 × 几百字节 =
 * KB 级数据、无查询无索引需求、唯一消费方是本进程——XML 文件（App
 * 私有目录，落在手表闪存）+ JSON 数组零依赖零 schema 管理，全量读写
 * 瞬间完成。数据形态变了直接整体丢弃重建（catch 后返回空），不做
 * 字段级迁移——历史消息不是资产，坏了大不了重来。
 */
class MsgStore(private val prefs: SharedPreferences) : MsgLog.Persist {

    override fun save(msgs: List<MsgLog.Msg>) {
        val arr = JSONArray()
        for (m in msgs) {
            arr.put(
                JSONObject()
                    .put("id", m.id)
                    .put("ts", m.tsMs)
                    .put("ask", m.ask)
                    .put("reply", m.reply)
                    .put("msg_id", m.msgId)
                    .put("pending", m.pending)
                    .put("failed", m.failed)
            )
        }
        prefs.edit().putString(KEY, arr.toString()).apply()
    }

    override fun load(): List<MsgLog.Msg> {
        val raw = prefs.getString(KEY, null) ?: return emptyList()
        return try {
            val arr = JSONArray(raw)
            // 存储序 = 新→旧（save 的入参序）；load 契约要旧→新 → 反转
            buildList {
                for (i in 0 until arr.length()) {
                    val o = arr.getJSONObject(i)
                    add(
                        MsgLog.Msg(
                            id = o.getLong("id"),
                            tsMs = o.getLong("ts"),
                            ask = o.optString("ask"),
                            reply = o.optString("reply"),
                            msgId = o.optString("msg_id"),
                            pending = o.optBoolean("pending"),
                            failed = o.optString("failed"),
                        )
                    )
                }
            }.reversed()
        } catch (_: Exception) {
            emptyList()   // 数据坏了整体丢弃：消息历史不值得做字段级迁移
        }
    }

    companion object {
        private const val KEY = "msg_history"
    }
}
