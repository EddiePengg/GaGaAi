# 中文字体资产（gaga_font_cjk_16）

UI 中文用思源黑体子集（Noto Sans SC = Source Han Sans，**OFL 1.1 许可**，可自由子集化/
嵌入固件，只需保留许可声明）。

- 生成物：`src/ui/fonts/gaga_font_cjk_16.c`（LVGL C 数组字体，16px，bpp=4，提交入库）
- 字符集：`charset_cjk.txt`（**GB2312 全集 7450 字符**：汉字区 6763 + 符号区 681 + 补充 ·℉✓✗ 和 `—`(U+2014)`–`(U+2013)，2026-09-24 扩容重生成）
  - ⚠️ 破折号坑：GB2312 的 0xA1AA 经标准映射是 `―`(U+2015)，而 AI 输出的破折号是 `—`(U+2014)——形同码异，
    字符集按 GB2312 收全会漏掉它（2026-09-24 真机报障"逐字对比两横线显示不出"的根因），补充清单里必须有 U+2014
- 字体源文件：NotoSansSC[wght].ttf（Google Fonts 官方仓库，**不提交**——17.7MB，
  重新生成时再下载）：
  <https://raw.githubusercontent.com/google/fonts/main/ofl/notosanssc/NotoSansSC%5Bwght%5D.ttf>
- OFL 许可文本：<https://openfontlicense.org>（重新分发生成物时附上本说明即满足要求）

## 重新生成（改了 charset_cjk.txt 或换字号后）

```bash
npm install lv_font_conv   # 任意目录
lv_font_conv \
  --font /path/to/NotoSansSC.ttf \
  --size 16 --bpp 4 --format lvgl \
  --range 0x20-0x7E \
  --symbols "$(cat charset_cjk.txt)" \
  --lv-font-name gaga_font_cjk_16 \
  -o ../../src/ui/fonts/gaga_font_cjk_16.c
# 然后把生成文件里的条件 include 段改为一行 #include "lvgl.h"（本工程布局）
```

注意：动态文本（talk_asr/talk_reply/reply）可能含子集外汉字——按词频补进
charset_cjk.txt 重新生成即可，不要改用全量 CJK（flash 会爆）。
