# 见 dev-log：从用户 mp4 提取鸭叫采样 → src/audio/snd_quack.c
# 依赖：ffmpeg 转出的 /tmp/duck_raw.pcm（ffmpeg -i 源.mp4 -ar 16000 -ac 1 -f s16le）
# 实现：能量分段选最响亮一声，前后留 25/60ms，归一化到 26000 峰值
