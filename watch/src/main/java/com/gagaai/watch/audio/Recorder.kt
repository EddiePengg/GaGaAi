package com.gagaai.watch.audio

import android.annotation.SuppressLint
import android.media.AudioFormat
import android.media.AudioRecord
import android.media.MediaRecorder
import android.os.Process
import android.util.Log

/**
 * 录音器：麦克风 → PCM16 @16kHz mono → Opus 包。
 *
 * 音源用 VOICE_RECOGNITION：绕开系统的 AGC/降噪，留给服务端 Whisper 一份
 * 尽量"原味"的语音（与 ESP32 端保持一致的处理哲学）。
 *
 * 帧节奏：20ms 一包（320 样本 = 640 字节 PCM16）。Opus 包通过 [onOpusPacket]
 * 同步回调到调用线程，调用方负责组帧上行。
 */
class Recorder(
    private val onOpusPacket: (ByteArray) -> Unit,
    private val onError: (String) -> Unit,
) {
    companion object {
        private const val TAG = "GagaRecorder"
        const val SAMPLE_RATE = 16000
        const val FRAME_SAMPLES = 320                    // 20ms @16kHz
        const val FRAME_BYTES = FRAME_SAMPLES * 2        // PCM16
    }

    private var recordThread: Thread? = null
    private var encoder: OpusEncoder? = null

    @Volatile
    private var running = false

    val isRunning: Boolean get() = running

    /** 开始录音。已在录音时直接返回（幂等）。 */
    @SuppressLint("MissingPermission")
    fun start() {
        if (running) return
        if (!OpusEncoder.isSupported()) {
            onError("本机不支持 Opus 编码")
            return
        }
        running = true
        recordThread = Thread({
            Process.setThreadPriority(Process.THREAD_PRIORITY_URGENT_AUDIO)
            try {
                runCapture()
            } catch (t: Throwable) {
                Log.e(TAG, "capture failed", t)
                onError(t.message ?: "录音失败")
            } finally {
                running = false
            }
        }, "gaga-recorder").also { it.start() }
    }

    private fun runCapture() {
        val minBuf = AudioRecord.getMinBufferSize(
            SAMPLE_RATE,
            AudioFormat.CHANNEL_IN_MONO,
            AudioFormat.ENCODING_PCM_16BIT,
        )
        // 给 2 倍余量，避免低配手表 USB/总线抖动导致读超时
        val bufSize = maxOf(minBuf * 2, FRAME_BYTES * 8)

        @SuppressLint("MissingPermission")
        val rec = AudioRecord(
            MediaRecorder.AudioSource.VOICE_RECOGNITION,
            SAMPLE_RATE,
            AudioFormat.CHANNEL_IN_MONO,
            AudioFormat.ENCODING_PCM_16BIT,
            bufSize,
        )
        if (rec.state != AudioRecord.STATE_INITIALIZED) {
            rec.release()
            onError("麦克风初始化失败")
            return
        }

        val enc = OpusEncoder(SAMPLE_RATE, 1)
        enc.onPacket = { pkt -> onOpusPacket(pkt) }
        enc.start()
        encoder = enc

        val buf = ByteArray(FRAME_BYTES)
        try {
            rec.startRecording()
            while (running) {
                var read = 0
                while (read < FRAME_BYTES && running) {
                    val n = rec.read(buf, read, FRAME_BYTES - read)
                    if (n <= 0) break
                    read += n
                }
                if (read == FRAME_BYTES) {
                    enc.feed(buf, read)
                } else if (read > 0) {
                    // 不满一帧的尾巴：凑不齐 20ms 就直接丢，避免短包被服务端误判
                    break
                } else {
                    break
                }
            }
        } finally {
            try {
                rec.stop()
            } catch (_: Throwable) {
            }
            rec.release()
            enc.flush()   // v0.2.1：把编码器里压着的最后一包吐出来（结尾丢字修复）
            enc.stop()
            encoder = null
        }
    }

    /** 停止录音并释放麦克风。回调线程返回（join 由调用方自行决定是否等待）。 */
    fun stop() {
        running = false
        // 不 join：调用方可能是 UI 线程，join 会卡顿；线程自己在 finally 里清理
    }
}
