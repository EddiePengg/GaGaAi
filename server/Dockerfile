FROM python:3.12-slim

# Opus 解码依赖（ADR-014 相关：ffmpeg 子进程方案）
RUN apt-get update \
    && apt-get install -y --no-install-recommends ffmpeg \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY pyproject.toml ./
COPY src ./src
RUN pip install --no-cache-dir .

ENV PYTHONUNBUFFERED=1
# 首次启动会下载 whisper 模型（compose 里挂了 huggingface cache volume）
CMD ["gaga-server"]
