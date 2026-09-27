package com.gagaai.watch.audio

import android.content.Context
import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioManager
import android.media.AudioTrack
import android.media.SoundPool
import com.gagaai.watch.R
import java.util.concurrent.LinkedBlockingQueue
import kotlin.math.PI
import kotlin.math.sin

/**
 * 提示音播放器：与 esp32-idf/src/audio/AudioPipe.cpp 的"鸭子声音语言"同源
 *（2026-09-25 用户定稿，音效方案选择已废除）——
 *
 *   开始录音 / 发送完  = 嘎（真鸭叫采样，1 次）
 *   收到回复          = 叮咚双音（高→低，钉钉音同款）
 *   出错              = 嘌嘌（鸭子放屁，低频锯齿随机游走）
 *
 * 为什么不是清一色"嘎"：用户反馈会误以为重新录了一遍，其实是来消息——
 * 声数 = 事件类别，闭眼数数即可。
 *
 * 播放走后台线程 + 作业队列（对齐 ESP32 的 beepQueue）：UI 线程不阻塞。
 * "嘎"用 SoundPool（采样短、可重叠、低延迟）；"叮咚/嘌嘌"是合成音，
 * 用 AudioTrack 现算——它们没有素材，跟 ESP32 一样按公式合成，两端听感一致。
 */
class Sounds(private val context: Context) {

    private enum class Kind { QUACK, CHIME, POOT }

    private val queue = LinkedBlockingQueue<Kind>()
    private val thread: Thread
    private var quackId = 0
    private val soundPool = SoundPool.Builder()
        .setMaxStreams(2)
        .setAudioAttributes(
            AudioAttributes.Builder()
                .setUsage(AudioAttributes.USAGE_ASSISTANCE_SONIFICATION)
                .setContentType(AudioAttributes.CONTENT_TYPE_SONIFICATION)
                .build()
        )
        .build()

    @Volatile
    var enabled = true

    init {
        quackId = soundPool.load(context, R.raw.snd_quack, 1)
        thread = Thread({
            while (true) {
                val kind = queue.take()
                if (!enabled) continue
                try {
                    when (kind) {
                        Kind.QUACK -> playQuack()
                        Kind.CHIME -> playChime()
                        Kind.POOT -> playPoot()
                    }
                } catch (_: Throwable) {
                    // 提示音失败不许拖垮主流程
                }
            }
        }, "gaga-sounds").apply {
            isDaemon = true
            start()
        }
    }

    /** 开始录音 / 发送完 */
    fun quack() = queue.offer(Kind.QUACK)

    /** 收到回复（叮咚双音，钉钉音同款） */
    fun chime() = queue.offer(Kind.CHIME)

    /** 出错 */
    fun poot() = queue.offer(Kind.POOT)

    private fun playQuack() {
        // SoundPool 播放：左满右满、优先级 1、循环 0 次、速率 1.0
        soundPool.play(quackId, 1f, 1f, 1, 0, 1.0f)
        Thread.sleep(350)  // 让尾音落完，别被下一个作业掐断
    }

    /**
     * "叮咚"双音——回复到达。ESP32 AudioPipe.cpp case 3 同参数：
     *   1318Hz (E6) 120ms → 988Hz (B5) 180ms，线性衰减包络
     * 线性衰减包络听感比方波头尾"啪"干净（ESP32 注释原话）。
     */
    private fun playChime() {
        val freqs = floatArrayOf(1318f, 988f)
        val durMs = intArrayOf(120, 180)
        val total = freqs.indices.sumOf { SAMPLERATE * durMs[it] / 1000 }
        val pcm = ShortArray(total)
        var pos = 0
        for (n in freqs.indices) {
            val seg = SAMPLERATE * durMs[n] / 1000
            for (i in 0 until seg) {
                val t = i.toFloat() / SAMPLERATE
                val env = 1f - i.toFloat() / seg
                pcm[pos + i] = (sin(2 * PI * freqs[n] * t) * AMP * env * 32767).toInt().toShort()
            }
            pos += seg
        }
        playPcm(pcm)
    }

    /**
     * "嘌嘌"——错误专属。70~130Hz 锯齿随机游走 + 噗噤断续包络，两声。
     * 与"嘎"声家族（有音高）是两个声音物种，出错时不可能听错（ESP32 注释原话）。
     * 伪随机种子固定 20260925：嘌没有标准音，每次略不同反而真实。
     */
    private fun playPoot() {
        var seed = 20260925
        fun rnd(): Float {
            seed = seed * 1103515245 + 12345
            return ((seed ushr 16) and 0x7FFF) / 32768f
        }
        val per = SAMPLERATE / 5  // 每声 200ms
        val pcm = ShortArray(per * 2)
        var phase = 0f
        for (i in pcm.indices) {
            val t = i.toFloat() / SAMPLERATE
            val seg = if (t < 0.2f) 0 else 1
            val local = t - seg * 0.2f
            val f = 70f + 60f * rnd()
            phase += f / SAMPLERATE
            if (phase > 1f) phase -= 1f
            val saw = phase * 2f - 1f
            // 噗噤断续包络：前 2/3 强、后 1/3 快速收
            val env = if (local < 0.13f) 1f else (0.2f - local) / 0.07f
            pcm[i] = (saw * env * AMP * 0.9f * 32767).toInt().toShort()
        }
        playPcm(pcm)
    }

    private fun playPcm(pcm: ShortArray) {
        val minBuf = AudioTrack.getMinBufferSize(
            SAMPLERATE, AudioFormat.CHANNEL_OUT_MONO, AudioFormat.ENCODING_PCM_16BIT
        )
        val track = AudioTrack(
            AudioManager.STREAM_MUSIC, SAMPLERATE, AudioFormat.CHANNEL_OUT_MONO,
            AudioFormat.ENCODING_PCM_16BIT, maxOf(minBuf, pcm.size * 2), AudioTrack.MODE_STATIC
        )
        try {
            track.write(pcm, 0, pcm.size)
            track.play()
            // 等到播放位走完再释放，否则尾音被截
            val ms = pcm.size * 1000L / SAMPLERATE + 50
            Thread.sleep(ms)
        } finally {
            try {
                track.release()
            } catch (_: Throwable) {
            }
        }
    }

    companion object {
        // 与 ESP32 端 RATE_REC 一致：16kHz
        private const val SAMPLERATE = 16000
        // 与 ESP32 端 BEEP_AMP 一致：0.5
        private const val AMP = 0.5f
    }
}
