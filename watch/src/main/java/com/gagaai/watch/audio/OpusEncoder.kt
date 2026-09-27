package com.gagaai.watch.audio

import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaFormat
import android.os.Build

/**
 * Android MediaCodec Opus 编码器封装。
 *
 * 为什么不用 JNI 拉 libopus：框架阶段先把链路搭通，系统编码器够用且省事；
 * 如果真机上发现系统 Opus 编码器帧粒度不稳定，再换 libopus JNI 不迟。
 *
 * 输入：16kHz 单声道 PCM16（与 ESP32 端 es7320+es8311 链路的采样率一致）
 * 输出：裸 Opus 包（**不是 Ogg**，协议 type=0x01 载的就是它）
 *
 * 可用性：Android 10（API 29）起系统才有 audio/opus 编码器。minSdk=26
 * 所以必须运行时探测，不可用就把错误直接抛给 UI 提示用户。
 */
class OpusEncoder(private val sampleRate: Int = 16000, private val channels: Int = 1) {

    companion object {
        /** 探测本机是否支持 Opus 编码（不实际创建 codec，只查列表，便宜） */
        fun isSupported(): Boolean {
            if (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q) return false
            return try {
                android.media.MediaCodecList(android.media.MediaCodecList.REGULAR_CODECS)
                    .codecInfos
                    .any { info ->
                        info.isEncoder && info.supportedTypes.any {
                            it.equals("audio/opus", ignoreCase = true)
                        }
                    }
            } catch (_: Throwable) {
                false
            }
        }
    }

    private var codec: MediaCodec? = null
    private val bufferInfo = MediaCodec.BufferInfo()

    /** 单包 Opus 输出回调（在调用 [feed] 的线程上同步触发） */
    var onPacket: ((ByteArray) -> Unit)? = null

    fun start() {
        val format = MediaFormat.createAudioFormat(MediaFormat.MIMETYPE_AUDIO_OPUS, sampleRate, channels)
        format.setInteger(MediaFormat.KEY_BIT_RATE, 24_000)         // 24kbps：语音足够，省流量
        format.setInteger(MediaFormat.KEY_MAX_INPUT_SIZE, 640)      // 20ms @16kHz mono PCM16
        format.setInteger(MediaFormat.KEY_COMPLEXITY, 5)            // 编码复杂度：平衡延迟与质量
        codec = MediaCodec.createEncoderByType(MediaFormat.MIMETYPE_AUDIO_OPUS).also {
            it.configure(format, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
            it.start()
        }
    }

    /**
     * 喂一段 PCM16（推荐恰好 20ms = 320 样本 = 640 字节）。
     * 编码器吐出的每个 output buffer 视作一个独立 Opus 包，回调 [onPacket]。
     */
    fun feed(pcm: ByteArray, length: Int = pcm.size) {
        val c = codec ?: return
        val inIdx = c.dequeueInputBuffer(10_000)
        if (inIdx < 0) return
        val inBuf = c.getInputBuffer(inIdx) ?: return
        inBuf.clear()
        inBuf.put(pcm, 0, length)
        c.queueInputBuffer(inIdx, 0, length, System.nanoTime() / 1000, 0)
        drain()
    }

    private fun drain() {
        val c = codec ?: return
        while (true) {
            val outIdx = c.dequeueOutputBuffer(bufferInfo, 0)
            when {
                outIdx == MediaCodec.INFO_TRY_AGAIN_LATER -> return
                outIdx == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED -> continue
                outIdx >= 0 -> {
                    val outBuf = c.getOutputBuffer(outIdx) ?: return
                    if (bufferInfo.size > 0) {
                        val pkt = ByteArray(bufferInfo.size)
                        outBuf.position(bufferInfo.offset)
                        outBuf.get(pkt)
                        onPacket?.invoke(pkt)
                    }
                    c.releaseOutputBuffer(outIdx, false)
                }
                else -> return
            }
        }
    }

    /**
     * 收尾冲刷（v0.2.1）：编码器内部可能还压着最后一包没吐——停录时不冲，
     * 语句末尾 ~20-60ms 的字就没了（ASR 结尾丢字的隐蔽来源）。发 EOS 让
     * 编码器把残余全部吐完，最多等 200ms。
     */
    fun flush() {
        val c = codec ?: return
        try {
            val inIdx = c.dequeueInputBuffer(10_000)
            if (inIdx >= 0) {
                c.queueInputBuffer(inIdx, 0, 0, System.nanoTime() / 1000,
                    MediaCodec.BUFFER_FLAG_END_OF_STREAM)
            }
            val deadline = System.currentTimeMillis() + 200
            while (System.currentTimeMillis() < deadline) {
                val outIdx = c.dequeueOutputBuffer(bufferInfo, 10_000)
                if (outIdx >= 0) {
                    val outBuf = c.getOutputBuffer(outIdx)
                    if (bufferInfo.size > 0 && outBuf != null) {
                        val pkt = ByteArray(bufferInfo.size)
                        outBuf.position(bufferInfo.offset)
                        outBuf.get(pkt)
                        onPacket?.invoke(pkt)
                    }
                    val eos = bufferInfo.flags and MediaCodec.BUFFER_FLAG_END_OF_STREAM != 0
                    c.releaseOutputBuffer(outIdx, false)
                    if (eos) break
                }
            }
        } catch (_: Throwable) {
            // 冲刷是尽力而为：失败不致命（顶多丢尾巴一包）
        }
    }

    /** 停止并释放。调用后本实例不可复用。 */
    fun stop() {
        try {
            codec?.stop()
        } catch (_: Throwable) {
        }
        try {
            codec?.release()
        } catch (_: Throwable) {
        }
        codec = null
    }
}
