"""libopus 的 ctypes 最小封装：Opus 裸包 ↔ PCM16，服务端转码专用。

背景（ADR-035）：设备上行 Opus 16k/20ms，Step/Gemini 后端只收 PCM16；
下行 PCM 24k 也要转回 Opus 喂设备。pyogg 0.6.14a1 是半成品（__init__ 不导出
OpusEncoder/Decoder、opus.py 引用未定义的 c_int_p、类壳没有 encode/decode），
弃用；libopus 本体（brew install opus）直接 ctypes 调，零第三方依赖。

封装面（只服务实时转码，不做文件容器）：
    OpusCodecEncoder(fs, channels)   .encode(pcm_bytes) -> opus packet bytes
    OpusCodecDecoder(fs, channels)   .decode(opus_packet) -> pcm bytes (bytearray)
帧长固定 20ms（设备链路约定）。
"""
from __future__ import annotations

import ctypes

# libopus 常量
_OPUS_APPLICATION_VOIP = 2048
_OPUS_APPLICATION_AUDIO = 2049
_OPUS_SET_BITRATE_REQUEST = 4002
_OPUS_RESET_STATE_REQUEST = 4028

_CANDIDATE_LIBS = (
    "/opt/homebrew/lib/libopus.0.dylib",   # macOS arm64 brew
    "/usr/local/lib/libopus.0.dylib",      # macOS x86 brew
    "libopus.so.0",                        # Linux（dokploy/Debian）
    "libopus.so",
)

_lib = None


def _load() -> ctypes.CDLL:
    global _lib
    if _lib is not None:
        return _lib
    last_err: Exception | None = None
    for cand in _CANDIDATE_LIBS:
        try:
            _lib = ctypes.CDLL(cand)
            break
        except OSError as e:
            last_err = e
    if _lib is None:
        raise RuntimeError(
            f"libopus 加载失败（{last_err}）——brew install opus / apt install libopus0")
    # 函数签名（只声明用到的）
    _lib.opus_encoder_create.restype = ctypes.c_void_p
    _lib.opus_encoder_create.argtypes = [ctypes.c_int, ctypes.c_int,
                                         ctypes.c_int, ctypes.POINTER(ctypes.c_int)]
    _lib.opus_encode.restype = ctypes.c_int
    _lib.opus_encode.argtypes = [ctypes.c_void_p,
                                 ctypes.POINTER(ctypes.c_int16),
                                 ctypes.c_int, ctypes.c_char_p,
                                 ctypes.c_int32]
    _lib.opus_decoder_create.restype = ctypes.c_void_p
    _lib.opus_decoder_create.argtypes = [ctypes.c_int, ctypes.c_int,
                                         ctypes.POINTER(ctypes.c_int)]
    _lib.opus_decode.restype = ctypes.c_int
    _lib.opus_decode.argtypes = [ctypes.c_void_p,
                                 ctypes.c_char_p, ctypes.c_int32,
                                 ctypes.POINTER(ctypes.c_int16),
                                 ctypes.c_int, ctypes.c_int]
    _lib.opus_encoder_ctl.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int32]
    return _lib


class OpusCodecError(Exception):
    pass


class OpusCodecEncoder:
    def __init__(self, fs: int = 24000, channels: int = 1,
                 bitrate_bps: int = 24000):
        lib = _load()
        err = ctypes.c_int(0)
        self._ch = channels
        self._enc = lib.opus_encoder_create(fs, channels,
                                            _OPUS_APPLICATION_VOIP,
                                            ctypes.byref(err))
        if err.value != 0 or not self._enc:
            raise OpusCodecError(f"opus_encoder_create 失败 code={err.value}")
        # 码率：24kbps ≈ 设备上行档，语音够用
        lib.opus_encoder_ctl(self._enc, _OPUS_SET_BITRATE_REQUEST, bitrate_bps)
        self._frame_samples = fs * 20 // 1000  # 20ms

    def encode(self, pcm: bytes) -> bytes:
        """一帧 20ms PCM16LE → Opus 裸包。样本数不对整帧会失败（调用方保证）。"""
        lib = _load()
        n = len(pcm) // (2 * self._ch)
        if n != self._frame_samples:
            raise OpusCodecError(
                f"encode 需要 {self._frame_samples} 采样/声道，收到 {n}")
        out = ctypes.create_string_buffer(400)  # opus 单包上限 1275，实际远小
        ret = lib.opus_encode(
            self._enc, ctypes.cast(pcm, ctypes.POINTER(ctypes.c_int16)),
            n, out, len(out))
        if ret < 0:
            raise OpusCodecError(f"opus_encode 失败 code={ret}")
        return out.raw[:ret]


class OpusCodecDecoder:
    def __init__(self, fs: int = 16000, channels: int = 1):
        lib = _load()
        err = ctypes.c_int(0)
        self._ch = channels
        self._frame_samples = fs * 20 // 1000
        self._dec = lib.opus_decoder_create(fs, channels, ctypes.byref(err))
        if err.value != 0 or not self._dec:
            raise OpusCodecError(f"opus_decoder_create 失败 code={err.value}")
        self._out = ctypes.create_string_buffer(
            self._frame_samples * 2 * channels)

    def decode(self, packet: bytes) -> bytes:
        """一个 20ms Opus 裸包 → PCM16LE 字节。损坏/丢包出静音帧（PLC）。"""
        lib = _load()
        ret = lib.opus_decode(
            self._dec,
            ctypes.cast(packet, ctypes.c_char_p) if packet else None,
            len(packet),
            ctypes.cast(self._out, ctypes.POINTER(ctypes.c_int16)),
            self._frame_samples, 0)
        if ret < 0:
            raise OpusCodecError(f"opus_decode 失败 code={ret}")
        return self._out.raw[:ret * 2 * self._ch]
