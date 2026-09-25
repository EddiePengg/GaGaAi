// i18n：单文件字典方案（ADR-040 修订）。
// - 静态 HTML 即默认英文：无 JS、爬虫、首次绘制看到的都是英文；
// - 切换优先级：URL ?lang= > localStorage('gaga-lang') > 英文（站点默认英语）；
// - 字典是第一方可信内容，故用 innerHTML（部分文案含 <br>/<small>/<code>/span）；
// - 加新语言：在 DICT 加一个键值对象，并在 LANGS 与导航语言钮里登记即可。
(function () {
  'use strict';

  var LANGS = ['en', 'zh'];
  var STORE_KEY = 'gaga-lang';

  var DICT = {
    en: {
      'meta.title': 'gaga ai · an AI portal on your chest',
      'a11y.skip': 'Skip to content',
      'a11y.lang': 'Language',
      'nav.how': 'How it works',
      'nav.features': 'Features',
      'nav.hardware': 'Hardware',
      'nav.roadmap': 'Roadmap',
      'nav.soon': 'Coming soon',
      'hero.title1': 'GAGA',
      'hero.scroll': 'Scroll on — see what this goose is up to',
      'intro.kicker': 'Grasp up, Get AI · a one-hand AI portal on your chest 🦆',
      'intro.title': 'One hand talks.<br>The other keeps working.',
      'intro.lede': 'gaga ai is an AI portal you wear on your chest: hold a button, speak, and your words land as text in your Feishu group. Hermes answers on the spot, and the device chirps the reply back to you. Riding, cooking, carrying stuff — your hands never stop.',
      'intro.cta.how': 'See how it works',
      'intro.cta.roadmap': 'View the roadmap',
      'stat1.dt': 'Speech → Feishu text',
      'stat1.small': 'Streaming ASR, straight through',
      'stat2.dt': 'The full loop',
      'stat2.small': 'Core promise shipped',
      'stat3.dt': 'Round display',
      'stat4.dt': 'Uplink framing',
      'term.aria': 'Demo: after a push-to-talk, speech is transcribed and sent to Feishu; Hermes replies and the device gets a receipt',
      'term.body': '<span class="t-dim">$</span> bottom-right key · hold to talk\n  <span class="t-accent">beep</span> → “Can I go biking today?” → <span class="t-accent">beep</span>\n\n<span class="t-ok">✓</span> streaming ASR · <span class="t-num">1.9s</span>\n<span class="t-ok">→</span> Feishu · Hermes\n  Sure! Wind drops after 6 pm —<br>  helmet on and go 🦆\n\n<span class="t-ok">✓</span> receipt · the device chirps · <span class="t-dim">reply on your card</span>\n\n<span class="t-duck">   __\n &lt;(o )___\n  ( ._&gt; /\n   `---\'</span>',
      'how.title': 'How it works',
      'how.lede': 'Five steps, from your mouth to the group chat and back to your chest. The app is a dumb pipe that understands nothing — every bit of complexity lives on the server.',
      'how.s1.t': 'Hold to talk',
      'how.s1.p': 'Long-press the bottom-right key: beep → speak → release to send. A 500&thinsp;ms release grace absorbs grip wobble.',
      'how.s2.t': 'BLE → App',
      'how.s2.p': 'Bluetooth hands 20&thinsp;ms Opus frames to the phone app; the app forwards bytes and understands none of it.',
      'how.s3.t': 'The server',
      'how.s3.p': 'The only brain: always-on MQTT signaling, streaming ASR, sessions and tools all live here.',
      'how.s4.t': 'Feishu · Hermes',
      'how.s4.p': 'Text lands in the group and the agent replies. The channel layer is abstracted — WeChat and Telegram can plug in later.',
      'how.s5.t': 'Receipt downlink',
      'how.s5.p': 'The device chirps; the recognized text and the reply show up as cards — the screen even lights up for them.',
      'how.note': 'Light signaling always on, heavy media only when needed — between talks, just a heartbeat of a few dozen bytes stays alive. That is how a pendant survives the day. Home-Wi-Fi direct mode is on the roadmap.',
      'feat.title': 'All from one hand',
      'feat.lede': 'Six features that grew out of a “my hands are full” life.',
      'feat.1.t': 'Push to talk',
      'feat.1.p': 'Like a walkie-talkie: hold, speak, let go. A red dot with a timer while recording, then “sending → ✅”.',
      'feat.2.t': 'Realtime full duplex',
      'feat.2.p': 'Long-press the top-right key and just talk, straight into Volcano Realtime; toggle subtitles to follow every word.',
      'feat.3.t': 'Replies as cards',
      'feat.3.p': 'A message-card list plus a status bar; the full GB2312 glyph set (8,000+ codepoints) means Chinese replies never lose a character.',
      'feat.4.t': 'A dumb pipe, for privacy',
      'feat.4.p': 'The app only forwards BLE↔MQTT bytes and understands none of it; no secrets ever live on the device.',
      'feat.5.t': 'The server is the only brain',
      'feat.5.p': 'Auth, ASR, tool calls and logs all live server-side. New features touch only the server; the device stays boring-stable.',
      'feat.6.t': 'Every milliamp counts',
      'feat.6.p': 'Power-on on demand, downclock with the screen off, deep sleep when idle; AMOLED black pixels draw nothing — the deep-black theme saves by design.',
      'hw.title': 'The hardware on your chest',
      'hw.lede': 'A Waveshare ESP32-S3 1.75″ AMOLED round touch board in an aluminum case with a lanyard — every pin and driver copied from official sources, nothing guessed.',
      'hw.k1': 'MCU', 'hw.k2': 'Storage', 'hw.k3': 'Display', 'hw.k4': 'Touch',
      'hw.k5': 'Microphones', 'hw.k6': 'Speaker', 'hw.k7': 'Motion', 'hw.k8': 'Power',
      'hw.k9': 'Wearing', 'hw.k10': 'Keys',
      'hw.v1': 'ESP32-S3R8 · dual-core 240&thinsp;MHz · 8&thinsp;MB PSRAM',
      'hw.v5': 'Dual mics · ES7210 · hardware AEC',
      'hw.v6': 'ES8311 + NS4150B amp',
      'hw.v7': 'QMI8658 6-axis · in-sensor double-tap engine',
      'hw.v8': 'AXP2101 PMU · 400&thinsp;mAh Li-ion · Type-C',
      'hw.v9': 'Aluminum case + lanyard, worn on the chest',
      'hw.v10': 'Top right: wake / hold to chat · Bottom right: hold to talk',
      'rm.title': 'Roadmap',
      'rm.lede': 'Kept in sync with <code>docs/roadmap.md</code> in the repo — only the main line lives here.',
      'badge.done': '✓ Done',
      'badge.wip': '◐ In progress',
      'badge.plan': '○ Planned',
      'rm.m1.t': 'M1 · Server pipeline <small>2026-09-22</small>',
      'rm.m1.p': 'MQTT broker + streaming ASR (Volcano passthrough, 1.6–2.2&thinsp;s) + the Feishu channel — receipt loop closed.',
      'rm.m2.t': 'M2 · Android dumb pipe <small>2026-09-22</small>',
      'rm.m2.p': 'Foreground-service keep-alive stack, companion-device auto pairing, the MQTT ping-pong fix.',
      'rm.m3.t': 'M3 · Device firmware (Arduino / IDF) <small>2026-09-22–23</small>',
      'rm.m3.p': 'Board-isolated <code>variants/</code>, round display lit, audio uplink working; the IDF line reached a “minimum viable duck”.',
      'rm.m4.t': 'M4 · The full loop, closed <small>2026-09-23</small>',
      'rm.m4.p': 'Press, speak → text in Feishu → Hermes answers → the device chirps. The core promise, kept 🎉',
      'rm.m5.t': 'M5 · Realtime full-duplex chat',
      'rm.m5.p': 'Doubao Realtime bridged through two live conversations; AEC, barge-in and exit intent are being tuned.',
      'rm.m6.t': 'M6 · Experience layer',
      'rm.m6.p': 'Deep-black theme, settings, message cards, goose page and quick panel shipped; AOD watch face and IMU particles next.',
      'rm.next.t': 'Beyond',
      'rm.next.p': 'Home-Wi-Fi direct (no phone in the middle), keyword wake-up, multiple devices.',
      'soon.title': 'Coming soon',
      'soon.lede': 'Docs, tutorials and a community are on the way. The seats are reserved — content moves in as it is ready.',
      'soon.badge.wip': 'In the works',
      'soon.badge.plan': 'Planned',
      'soon.docs.t': 'Docs',
      'soon.docs.p': 'Architecture, protocol, data model and every decision record, being polished into a proper docs site.',
      'soon.docs.hint': 'For now, read <code>docs/</code> in the repo — the single source of truth.',
      'soon.tut.t': 'Tutorials',
      'soon.tut.p': 'From backing up the factory firmware to lighting the round screen and flying your first words into Feishu.',
      'soon.tut.hint': 'Written for a zero-to-demo afternoon.',
      'soon.comm.t': 'Community',
      'soon.comm.p': 'A group chat, feature discussions and a “show off your duck” column — channels will be announced right here.',
      'soon.comm.hint': 'Want to know first? Watch the repo.',
      'footer.brand': 'gaga ai · Grasp up, Get AI · the one-hand AI portal on your chest 🦆',
      'footer.meta': 'A static showcase page, kept in sync with <code>docs/</code> in the repo.<br>Hero goose photo from Wikimedia Commons (CC BY-SA 4.0)<br>© 2026 the gaga ai project'
    },
    zh: {
      'meta.title': '嘎嘎 AI · 把 AI 别在胸前',
      'a11y.skip': '跳到主要内容',
      'a11y.lang': '语言',
      'nav.how': '工作原理',
      'nav.features': '特性',
      'nav.hardware': '硬件',
      'nav.roadmap': '路线图',
      'nav.soon': '即将上线',
      'hero.title1': '嘎嘎',
      'hero.scroll': '往下看，这只鹅在做什么',
      'intro.kicker': 'Grasp up, Get AI · 胸前的单手 AI 入口 🦆',
      'intro.title': '另一只手，<br>不用停下来。',
      'intro.lede': 'gaga ai 是别在胸前的 AI 入口设备：按住按键说话，话音变成文字飞进飞书群，Hermes 当场回应，设备“叮”一声把回复递到你眼前。骑行、做饭、搬东西——双手忙，也不打断。',
      'intro.cta.how': '看看它怎么工作',
      'intro.cta.roadmap': '查看路线图',
      'stat1.dt': '说话到飞书出字',
      'stat1.small': '流式 ASR 直通',
      'stat2.dt': '全链路闭环',
      'stat2.small': '核心价值已达成',
      'stat3.dt': '圆屏',
      'stat4.dt': '上行分帧',
      'term.aria': '演示：按住说话后，语音被识别成文字发到飞书，Hermes 回复并回执到设备',
      'term.body': '<span class="t-dim">$</span> 右下键 · 长按说话\n  <span class="t-accent">滴滴</span> → 「今天适合骑车吗？」→ <span class="t-accent">滴滴</span>\n\n<span class="t-ok">✓</span> ASR 流式识别 · <span class="t-num">1.9s</span>\n<span class="t-ok">→</span> 飞书 · Hermes\n  适合！傍晚 6 点后风小于三级，<br>  戴上头盔出发 🦆\n\n<span class="t-ok">✓</span> receipt · 设备“叮”一声 · <span class="t-dim">回复已出卡</span>\n\n<span class="t-duck">   __\n &lt;(o )___\n  ( ._&gt; /\n   `---\'</span>',
      'how.title': '它是怎么工作的',
      'how.lede': '五步，从嘴里到群里再回到胸前。App 是只读不懂内容的哑管道，一切复杂度都收在服务器。',
      'how.s1.t': '按住说话',
      'how.s1.p': '右下键长按：滴滴 → 说话 → 松手即发送。松手宽限 500&thinsp;ms，握力波动不误断。',
      'how.s2.t': 'BLE → App',
      'how.s2.p': '蓝牙把 20&thinsp;ms 的 Opus 分帧交给手机 App；App 只转发字节，不理解任何内容。',
      'how.s3.t': '服务器',
      'how.s3.p': '唯一的大脑：MQTT 信令常连、流式 ASR 转文字、会话与工具全在这里。',
      'how.s4.t': '飞书 · Hermes',
      'how.s4.p': '文字进群，Agent 回应。接入端做了抽象，往后可扩微信、Telegram。',
      'how.s5.t': '回执下行',
      'how.s5.p': '设备“叮”一声，识别原文与回复就地出卡，息屏也会自动亮起。',
      'how.note': '轻信令常开，重媒体按需——平时只有几十字节的心跳活着，WebSocket 只在实时对话期间出现，这是挂坠省电的根基。在家 WiFi 直连模式已在规划中。',
      'feat.title': '一只手，全覆盖',
      'feat.lede': '为“手上正忙”的场景长出来的六个特性。',
      'feat.1.t': '按住说话',
      'feat.1.p': '像对讲机一样直觉：按住开口、松手即走。录音中红点计时，发送后“发送中 → ✅”。',
      'feat.2.t': '实时全双工对话',
      'feat.2.p': '长按右上开口即聊，火山 Realtime 直通；可开“对话字幕”跟读每一句。',
      'feat.3.t': '回复就地出卡',
      'feat.3.p': '消息卡列表 + 顶部状态栏；GB2312 全字库 8000+ 码点，中文回复不缺字。',
      'feat.4.t': '哑管道，保隐私',
      'feat.4.p': 'App 只转发 BLE↔MQTT 字节，读不懂内容；设备里不藏任何密钥。',
      'feat.5.t': '服务器是唯一大脑',
      'feat.5.p': '鉴权、ASR、工具分发、日志全在服务端；以后加功能只动服务器，设备稳到不用改。',
      'feat.6.t': '为挂坠省的每一毫安',
      'feat.6.p': '按需上电、息屏降频、挂机深睡；AMOLED 黑像素零功耗，深空黑主题天生省电。',
      'hw.title': '挂在胸前的硬件',
      'hw.lede': '微雪 ESP32-S3 1.75″ AMOLED 圆形触摸开发板，铝合金外壳 + 挂绳，引脚与驱动全部照抄官方资料，不猜一个脚位。',
      'hw.k1': '主控', 'hw.k2': '存储', 'hw.k3': '屏幕', 'hw.k4': '触摸',
      'hw.k5': '拾音', 'hw.k6': '放音', 'hw.k7': '姿态', 'hw.k8': '电源',
      'hw.k9': '佩戴', 'hw.k10': '按键',
      'hw.v1': 'ESP32-S3R8 · 双核 240&thinsp;MHz · 8&thinsp;MB PSRAM',
      'hw.v5': '双麦克风 · ES7210 · 硬件 AEC',
      'hw.v6': 'ES8311 + NS4150B 功放',
      'hw.v7': 'QMI8658 六轴 · 传感器内 Tap 双击引擎',
      'hw.v8': 'AXP2101 PMU · 400&thinsp;mAh 锂电 · Type-C',
      'hw.v9': '铝合金外壳 + 挂绳，胸前佩戴',
      'hw.v10': '右上：亮屏 / 长按对话 · 右下：按住说话',
      'rm.title': '路线图',
      'rm.lede': '进度与仓库 <code>docs/roadmap.md</code> 同步维护，这里只放主线。',
      'badge.done': '✓ 已完成',
      'badge.wip': '◐ 进行中',
      'badge.plan': '○ 规划中',
      'rm.m1.t': 'M1 · 服务端管道 <small>2026-09-22</small>',
      'rm.m1.p': 'MQTT Broker + 流式 ASR（火山直通，1.6–2.2&thinsp;s）+ 飞书接入，回执闭环。',
      'rm.m2.t': 'M2 · Android 哑管道 <small>2026-09-22</small>',
      'rm.m2.p': '前台服务保活组合拳、伴生设备自动配对、MQTT 乒乓互踢修复。',
      'rm.m3.t': 'M3 · 设备固件（Arduino / IDF 双线）<small>2026-09-22–23</small>',
      'rm.m3.p': '板级隔离 <code>variants/</code>，点亮圆屏、录音上行；IDF 线达成“最小可用鸭子”。',
      'rm.m4.t': 'M4 · 全链路闭环 <small>2026-09-23</small>',
      'rm.m4.p': '按一下说话 → 飞书出字 → Hermes 回应 → 设备“叮”。项目核心价值达成 🎉',
      'rm.m5.t': 'M5 · 实时全双工对话',
      'rm.m5.p': '豆包 Realtime 桥接已通两轮真实对话；AEC、打断支持、退出意图联调中。',
      'rm.m6.t': 'M6 · 体验层',
      'rm.m6.p': '深空黑主题、设置系统、消息卡、鸭子页、快捷面板已上线；AOD 表盘、IMU 粒子待做。',
      'rm.next.t': '之后',
      'rm.next.p': '在家 WiFi 直连（摆脱手机 BLE 依赖）、关键词唤醒、多设备支持。',
      'soon.title': '即将上线',
      'soon.lede': '文档、教程与社区都在路上。位置先占好，内容陆续搬进来。',
      'soon.badge.wip': '建设中',
      'soon.badge.plan': '规划中',
      'soon.docs.t': '文档',
      'soon.docs.p': '架构、通信协议、数据建模与全部决策记录，正在整理成正式文档站。',
      'soon.docs.hint': '现在可以先读仓库里的 <code>docs/</code>——那里是唯一事实来源。',
      'soon.tut.t': '教程',
      'soon.tut.p': '从开箱备份原厂固件，到点亮圆屏、第一句话飞进飞书的完整上手教程。',
      'soon.tut.hint': '会按“零基础一下午跑通”的节奏来写。',
      'soon.comm.t': '社区',
      'soon.comm.p': '交流群、功能讨论与“晒鸭”专栏，渠道开通后在这里公布。',
      'soon.comm.hint': '想第一时间知道？先关注仓库的更新。',
      'footer.brand': 'gaga ai · Grasp up, Get AI · 胸前的单手 AI 入口 🦆',
      'footer.meta': '本站为纯静态展示页，内容与仓库 <code>docs/</code> 同步维护。<br>首页大鹅照片来自 Wikimedia Commons（CC BY-SA 4.0）<br>© 2026 gaga ai 项目'
    }
  };

  function dictFor(lang) {
    return DICT[lang] || DICT.en;
  }

  function apply(lang) {
    var dict = dictFor(lang);
    document.querySelectorAll('[data-i18n]').forEach(function (el) {
      var key = el.getAttribute('data-i18n');
      if (dict[key] !== undefined) el.innerHTML = dict[key];
    });
    document.querySelectorAll('[data-i18n-aria]').forEach(function (el) {
      var key = el.getAttribute('data-i18n-aria');
      if (dict[key] !== undefined) el.setAttribute('aria-label', dict[key]);
    });
    document.documentElement.lang = lang === 'zh' ? 'zh-CN' : 'en';
    if (dict['meta.title']) document.title = dict['meta.title'];
    document.querySelectorAll('.lang-switch [data-lang]').forEach(function (btn) {
      var active = btn.getAttribute('data-lang') === lang;
      btn.classList.toggle('is-active', active);
      if (active) btn.setAttribute('aria-current', 'true');
      else btn.removeAttribute('aria-current');
    });
  }

  function initial() {
    var fromUrl = new URLSearchParams(location.search).get('lang');
    if (fromUrl && LANGS.indexOf(fromUrl) !== -1) return fromUrl;
    try {
      var saved = localStorage.getItem(STORE_KEY);
      if (saved && LANGS.indexOf(saved) !== -1) return saved;
    } catch (e) { /* 隐私模式下 localStorage 可能不可用 */ }
    return 'en';   // 站点默认英语
  }

  var current = initial();
  apply(current);

  document.addEventListener('click', function (e) {
    var btn = e.target.closest('[data-lang]');
    if (!btn) return;
    var lang = btn.getAttribute('data-lang');
    if (LANGS.indexOf(lang) === -1 || lang === current) return;
    current = lang;
    try { localStorage.setItem(STORE_KEY, lang); } catch (err) { /* 同上 */ }
    var url = new URL(location.href);
    url.searchParams.set('lang', lang);
    history.replaceState(null, '', url);
    apply(lang);
  });
})();
