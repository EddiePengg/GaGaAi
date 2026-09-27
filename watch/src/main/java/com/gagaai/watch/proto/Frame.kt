package com.gagaai.watch.proto

/**
 * 帧格式（与 esp32-idf/src/protocol/frame.cpp、server/src/gaga_server/frames.py 同源）：
 *
 *   [1B type][2B payload_len big-endian][payload]
 *
 * type 0x01 = Opus 音频包；0x02 = JSON 信令。
 *
 * 上行：手表直接走 MQTT，没有 MTU 限制，一律发**单包完整帧**（[encode]）。
 * 服务端 FrameAssembler 同时吃两种形态，不需要任何服务端改动。
 *
 * 下行：服务端按 512B/包分片（frames.py DEFAULT_MAX_PACKET_SIZE）——
 * LLM 回复的 reply 中文几百字（UTF-8 3B/字）几乎必然超 509B，所以
 * **下行必须用 [Reassembler] 重组**（v0.2.6：v0.2.5 只剥帧头不重组，
 * 分片首包 len=总长 > 单包实际字节，凑不齐直接丢 = reply 永远到不了，
 * 表现为"只能发不能收"且零报错）。
 */
object Frame {
    const val TYPE_CONTINUATION = 0x00
    const val TYPE_OPUS = 0x01
    const val TYPE_JSON = 0x02
    const val FLAG_MORE = 0x80

    /**
     * 编一帧单包完整字节，直接 publish 到 gaga/up。
     * payload 超过 65535 字节在 MQTT 场景下基本不会发生，仍按协议限制硬抛。
     */
    fun encode(type: Int, payload: ByteArray): ByteArray {
        require(payload.size <= 0xFFFF) { "payload too large: ${payload.size}" }
        val out = ByteArray(3 + payload.size)
        out[0] = type.toByte()
        out[1] = ((payload.size shr 8) and 0xFF).toByte()
        out[2] = (payload.size and 0xFF).toByte()
        System.arraycopy(payload, 0, out, 3, payload.size)
        return out
    }

    /** 编码一帧 JSON 信令（type=0x02）。 */
    fun encodeJson(json: String): ByteArray = encode(TYPE_JSON, json.toByteArray(Charsets.UTF_8))

    /**
     * 解一帧：返回 (type, payload)。字节不足/长度字段与实际不符返回 null。
     *
     * **为什么必须有它（v0.2.1 致命修复）**：服务端下发的 gaga/down 载荷
     * 是**带 3 字节帧头**的（mqtt_bridge.publish_json 走 encode_frame），
     * v0.2.0 把原始字节直接喂给 JSON 解析——首字节 0x02 不是合法 JSON，
     * **解析永远失败，receipt/reply/error 一条都到不了界面**（手表只能发、
     * 永远收不到，且无任何报错）。
     */
    fun decode(bytes: ByteArray): Pair<Int, ByteArray>? {
        if (bytes.size < 3) return null
        val type = bytes[0].toInt() and 0x7F   // 高位分片标志位不属于类型
        val len = ((bytes[1].toInt() and 0xFF) shl 8) or (bytes[2].toInt() and 0xFF)
        if (len < 0 || bytes.size < 3 + len) return null
        return type to bytes.copyOfRange(3, 3 + len)
    }

    /**
     * 解一个 MQTT 载荷里的**全部**帧（服务端一载荷一帧，但手机端合批经验
     * 在前，防御性按流迭代；残帧截断丢弃不留半帧）。
     */
    fun decodeAll(bytes: ByteArray): List<Pair<Int, ByteArray>> {
        val out = ArrayList<Pair<Int, ByteArray>>(1)
        var off = 0
        while (off + 3 <= bytes.size) {
            val type = bytes[off].toInt() and 0x7F
            val len = ((bytes[off + 1].toInt() and 0xFF) shl 8) or (bytes[off + 2].toInt() and 0xFF)
            if (off + 3 + len > bytes.size) break   // 残帧：丢弃
            out.add(type to bytes.copyOfRange(off + 3, off + 3 + len))
            off += 3 + len
        }
        return out
    }
}

/**
 * 下行分片重组器（协议 docs/protocol.md §2，语义对齐 server FrameReassembler /
 * esp32 FrameAssembler）：逐包喂入，凑齐一帧才交付。
 *
 *   首包 type|0x80，len = 整帧 payload 总长度
 *   中间包 type = 0x00，len = 本包字节数
 *   尾包 type = 原始 type，len = 本包字节数
 *   单包帧 type 不置 0x80，len = payload 长度
 *
 * 异常自保（孤儿续帧/超长/截断）静默丢弃残帧；首包后超时丢弃（5s）。
 * @Synchronized：HiveMQ publishes 回调线程与潜在的重连竞态下保持串行。
 */
class Reassembler(private val timeoutMs: Long = 5_000) {

    private var reassembling = false
    private var frameType = 0
    private var expected = 0
    private var buf = ByteArray(0)
    private var startedAt = 0L

    /** 喂一个 MQTT 载荷包；凑齐的完整帧在返回值里（一般 0 或 1 个）。 */
    @Synchronized
    fun feed(packet: ByteArray): List<Pair<Int, ByteArray>> {
        if (packet.size < 3) return emptyList()
        val now = System.currentTimeMillis()
        if (reassembling && now - startedAt > timeoutMs) reset()

        val typeField = packet[0].toInt() and 0xFF
        val lenField = ((packet[1].toInt() and 0xFF) shl 8) or (packet[2].toInt() and 0xFF)
        val body = packet.copyOfRange(3, packet.size)

        if (typeField and Frame.FLAG_MORE != 0) {
            // 首包：len 字段是整帧总长度（body 超出总长属对端异常，截断自保）
            reassembling = true
            frameType = typeField and 0x7F
            expected = lenField
            buf = body.copyOf(minOf(lenField, body.size))
            startedAt = now
            return if (buf.size >= expected) deliver() else emptyList()
        }
        if (typeField == Frame.TYPE_CONTINUATION) {
            if (!reassembling) return emptyList()   // 孤儿续帧：丢弃
            return append(body.copyOf(minOf(lenField, body.size)))
        }
        if (reassembling) {
            return append(body.copyOf(minOf(lenField, body.size)))   // 尾包
        }
        // 单包完整帧（服务端短信令的正常形态）
        if (lenField > body.size) return emptyList()
        return listOf(typeField to body.copyOf(lenField))
    }

    private fun append(data: ByteArray): List<Pair<Int, ByteArray>> {
        if (buf.size + data.size > expected) {
            reset()   // 超过首包宣告总长：对端不按协议来，丢弃残帧自保
            return emptyList()
        }
        buf += data
        return if (buf.size >= expected) deliver() else emptyList()
    }

    private fun deliver(): List<Pair<Int, ByteArray>> {
        val out = listOf(frameType to buf.copyOf(expected))
        reset()
        return out
    }

    private fun reset() {
        reassembling = false
        expected = 0
        buf = ByteArray(0)
    }
}
