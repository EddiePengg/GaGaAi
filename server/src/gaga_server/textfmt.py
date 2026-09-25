"""Markdown 拍平：Hermes 回复常带 Markdown，设备屏（圆屏 16px 单字号）渲染不动，
服务端拍平成短纯文本再下发（复杂度全压 server，宪法）。拍平规则（最简化版）：

- 围栏/行内代码去反引号留内容
- **粗体** *斜体* ~~删除~~ 去标记留内容
- # 标题 去 #；> 引用 去 >
- [文字](链接) → 文字；图片 ![alt](url) → alt
- 列表 - / * / 1. → "· "（有序保留 "1. " 序号）
- 表格行 | a | b | → "a / b"；分隔行 |---| 丢弃
- HTML 标签丢弃；空行压缩为单换行
"""
from __future__ import annotations

import re


def flatten_markdown(text: str) -> str:
    if not text:
        return ""
    s = text.replace("\r\n", "\n")

    # 围栏代码块：去 ```lang 围栏行，内容保留
    s = re.sub(r"^[ \t]*```[^\n]*\n?", "", s, flags=re.M)
    s = re.sub(r"^[ \t]*```[ \t]*$", "", s, flags=re.M)
    # HTML 标签丢弃
    s = re.sub(r"</?[a-zA-Z][^>]*>", "", s)
    # 图片 → alt；链接 → 文字
    s = re.sub(r"!\[([^\]]*)\]\([^)]*\)", r"\1", s)
    s = re.sub(r"\[([^\]]+)\]\([^)]*\)", r"\1", s)
    # 行内标记：反引号/粗斜体/删除线，留内容
    s = re.sub(r"`([^`]*)`", r"\1", s)
    s = re.sub(r"\*\*\*([^*]+)\*\*\*", r"\1", s)
    s = re.sub(r"\*\*([^*]+)\*\*", r"\1", s)
    s = re.sub(r"\*([^*]+)\*", r"\1", s)
    s = re.sub(r"~~([^~]+)~~", r"\1", s)

    out: list[str] = []
    for line in s.split("\n"):
        # 表格行 | a | b | → a / b；分隔行 |---| 丢弃
        if re.match(r"^\s*\|", line):
            cells = [c.strip() for c in line.strip().strip("|").split("|")]
            if all(re.fullmatch(r":?-{2,}:?", c) for c in cells if c):
                continue
            line = " / ".join(c for c in cells if c)
        # 标题去 #
        line = re.sub(r"^\s{0,3}#{1,6}\s+", "", line)
        # 引用去 >
        line = re.sub(r"^\s{0,3}>\s?", "", line)
        # 列表符号归一（有序保留序号）
        m = re.match(r"^(\s*)(\d+)[.)]\s+(.*)$", line)
        if m:
            line = f"{m.group(1)}{m.group(2)}. {m.group(3)}"
        else:
            line = re.sub(r"^\s{0,3}[-*+]\s+", "· ", line)
        # 水平线丢弃
        if re.fullmatch(r"\s*([-=_*])\1{2,}\s*", line):
            continue
        out.append(line.rstrip())

    # 空行压缩为单换行，头尾收干净
    flat = "\n".join(out)
    flat = re.sub(r"\n{3,}", "\n\n", flat)
    return flat.strip()
