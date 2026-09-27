package com.gagaai.watch.msg

import java.util.ArrayDeque

/**
 * 消息卡日志：一问一答一张卡，与 esp32-idf 的 MsgLog 同构。
 *
 * 卡片生命周期：松手即出占位卡（"识别中…"）→ receipt 填 ASR 文本 →
 * reply 就地填"嘎嘎"一栏。
 *
 * **持久化（v0.2.7）**：手表是随身设备，每次打开都清空历史不可接受——
 * 构造时从 [persist] 恢复、每次变更后全量落盘（几十条 KB 级，全量写
 * 无压力）。上次没等到结果的 pending 卡恢复时收尾成"超时"态：重启后
 * state=IDLE 不会再发 rec_status_query 对账，挂着只会永远"正在回复"。
 */
class MsgLog(
    capacity: Int = 20,
    private val persist: Persist? = null,
) {

    /** 持久化钩子：宿主决定存哪（手表用 SharedPreferences JSON，见 MsgStore）。 */
    interface Persist {
        /** 落盘，入参新→旧。 */
        fun save(msgs: List<Msg>)

        /** 恢复，返回**旧→新**（按时间序），条数截断由 MsgLog 的 capacity 管。 */
        fun load(): List<Msg>
    }

    /** 一张消息卡。text/ask/reply 随到达逐步填充。 */
    data class Msg(
        val id: Long,
        val tsMs: Long,
        var ask: String = "",        // 用户说的（ASR 终稿）
        var reply: String = "",      // 嘎嘎回的
        var msgId: String = "",      // 平台消息 id（reply.reply_to 用它配对）
        var pending: Boolean = true, // 还在等 receipt/reply
        var failed: String = "",     // 非空 = 发送失败原因
    )

    private val items = ArrayDeque<Msg>()
    private var nextId = 1L

    /** 容量（保留条数）。设置页可改，缩小时就地截断旧卡并落盘。 */
    var capacity = capacity
        set(value) {
            field = value
            trimAndPersist()
        }

    init {
        if (persist != null) {
            for (m in persist.load()) {
                m.pending = false   // 上次没等到结果的：恢复成"超时"态，别永远转圈
                items.addLast(m)
            }
            nextId = (items.maxOfOrNull { it.id } ?: 0L) + 1L
            trimAndPersist()
        }
    }

    /** 新建一张占位卡（松手即上屏），返回它。 */
    @Synchronized
    fun addPending(): Msg {
        val m = Msg(id = nextId++, tsMs = System.currentTimeMillis())
        items.addFirst(m)
        trimAndPersist()
        return m
    }

    /** receipt 到达：填 ASR 文本。按 msg_id 找卡，找不到就认最新那张 pending 的。 */
    @Synchronized
    fun fillAsk(msgId: String, text: String) {
        val m = findByMsgId(msgId) ?: findOldestPending() ?: return
        m.ask = text
        m.msgId = msgId
        trimAndPersist()
    }

    /** reply 到达：就地填回复。优先 reply_to 精确配对，miss 才 FIFO 兜底。 */
    @Synchronized
    fun fillReply(replyTo: String, text: String): Msg? {
        val m = findByMsgId(replyTo) ?: findOldestPending()
        m?.let {
            it.reply = text
            it.pending = false
        }
        trimAndPersist()
        return m
    }

    /** error 到达：标记失败。 */
    @Synchronized
    fun failLatest(reason: String) {
        findOldestPending()?.let {
            it.failed = reason
            it.pending = false
        }
        trimAndPersist()
    }

    /** 快照（新→旧），供 UI 渲染。 */
    @Synchronized
    fun snapshot(): List<Msg> = items.toList()

    /** 单条（详情页用）。 */
    @Synchronized
    fun get(id: Long): Msg? = items.find { it.id == id }

    private fun findByMsgId(msgId: String): Msg? =
        if (msgId.isEmpty()) null else items.find { it.msgId == msgId }

    private fun findOldestPending(): Msg? = items.lastOrNull { it.pending }

    private fun trimAndPersist() {
        while (items.size > capacity) items.removeLast()
        persist?.save(items.toList())
    }
}
