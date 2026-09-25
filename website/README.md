# website/ — gaga ai 展示站

纯静态单页展示站，无框架、无构建、无外部依赖（零 npm、零 CDN）。参考
[openclaw.ai](https://openclaw.ai) 的信息架构，视觉语言取自设备固件的
深空黑 + 鸭黄主题（ADR-031），吉祥物鸭子 🦆。

## 本地预览

```bash
cd website && python3 -m http.server 8787
# 打开 http://localhost:8787
```

（纯静态，双击 `index.html` 也能看，但走 HTTP 才有正确的字体/缓存行为。）

## 结构

```
website/
├── index.html        # 单页全部内容：大鹅 Hero / 简介 / 工作原理 / 特性 / 硬件 / 路线图 / 即将上线
└── assets/
    ├── style.css     # 全部样式；oklch 色板 token 化，组件只引用语义 token
    ├── i18n.js       # 中英字典 + 语言切换（默认英语，见下）
    ├── main.js       # 仅一处增强：滚动渐显（无 JS / prefers-reduced-motion 时内容直接可见）
    ├── goose.jpg     # 首页大鹅照片（1600×1200，≈390KB）
    └── favicon.png   # 品牌图标（白鹅）
```

## i18n（默认英语，可切中文）

- **静态 HTML 即英文**：无 JS、爬虫和首屏绘制的都是英文；`<html lang="en">`。
- **切换**：导航右侧 EN / 中文 分段钮。优先级 `?lang=` 参数 > `localStorage('gaga-lang')` > 英语。
  切换会同步 `<html lang>`、`document.title`、aria-label，并把选择写进 URL（可分享）。
- **字典**：全部文案在 `assets/i18n.js` 的 `DICT`（键 → `{en, zh}`），HTML 元素用
  `data-i18n="key"`（innerHTML，字典含 `<br>/<small>/<code>` 等）与
  `data-i18n-aria="key"`（aria-label）标注。字典是第一方内容，不存在注入面。
- **加新语言**：`DICT` 加一个对象 → `LANGS` 数组登记 → `.lang-switch` 里加一个
  `<button data-lang="xx">`，三步完事。
- **注意**：改中文文案要同时改 `DICT.zh` 与（如涉及默认展示）英文静态 HTML；
  改英文文案同样两处（静态 HTML + `DICT.en`），两处不一致会闪变。

## 首页大鹅（Hero）

- 来源：Wikimedia Commons ["A photo of a beautiful guard bird or goose with a Bokeh
  effect"](https://commons.wikimedia.org/wiki/File:A_photo_of_a_beautiful_guard_bird_or_goose_with_a_Bokeh_effect.png)，**CC BY-SA 4.0**。
- 已在两处署名：`index.html` hero 注释 + 页脚 meta 行。**换图时必须同步处理署名**。
- 处理链：Commons 1920px 缩略图 → `sips` 转 JPEG（1600 宽 / q75）。
- 融合手法：图片本体不透明，靠 `.hero__goose::after` 的四向渐变把照片底色压进
  深空黑页面底（右缘给文字让位、顶缘承接吸顶导航、底缘羽化防硬切边）；
  移动端渐变方向换成底部为主的另一组。
- 入场动画是纯 CSS 一次性 keyframes（`goose-in` / `rise-in`），包在
  `prefers-reduced-motion: no-preference` 里，不依赖 JS。

## 设计约定（改样式前必读）

- **色板实测过对比度**（WCAG）：正文对页面底 16.2:1、次文字 9.2:1、弱文字 5.8:1、
  鸭黄按钮深字 12.3:1，全部超出 4.5:1 要求。改任何一个色值都要重新量
  （oklch → sRGB → 相对亮度 → 比值），不许凭感觉换近似色。
- **语义 token**：组件里禁止出现裸 `oklch(...)`（鸭子插画与 focus ring 除外），
  只允许引用 `--color-*` / `--text-*` / `--space-*` / `--radius-*`。
- **同心圆角**：外圆角 = 内圆角 + padding（见 `.terminal__bar` 的 15px 注释）。
- **可访问性底线**：跳转链接、`lang="zh-CN"`、`role="img"` 的终端演示卡、
  44px 命中区、`:focus-visible` 鸭黄环、状态徽章"✓/◐/○ + 文字"不只靠颜色。
- **动效克制**：唯一的入场动画是滚动渐显 + 100ms 级联，包裹在
  `prefers-reduced-motion: no-preference` 里；按钮按压 scale(0.96)。

## 占位区块与后期规划

"即将上线"（`#soon`）三个卡片是占位空态，后期按下面路径填：

| 占位 | 后期形态 | 数据来源 |
|---|---|---|
| 文档 | 文档站（可用 MkDocs/VitePress 静态生成，挂 `/docs/` 路径） | `docs/` 全部内容 |
| 教程 | 图文 + 视频教程列表页 | 到手开箱/刷机/跑通全链路的实操记录 |
| 社区 | 交流群入口 + 讨论区链接 | 待定（群二维码、仓库 Discussions） |

加新占位卡时照抄 `.soon-card` 结构：说清楚这里将来是什么 + 现在能做什么，
不要放死链接。

## 内容同步铁律（AGENTS.md）

网站上的事实性内容（参数、进度、按键分工、里程碑状态）全部来自 `docs/`。
**改动 `docs/` 里被网站引用的内容时，必须同一次提交里同步改 `website/index.html`**，
反过来亦然。数据核对锚点：`docs/hardware.md`（规格）、`docs/roadmap.md`（进度）、
`README.md`（按键分工）。
