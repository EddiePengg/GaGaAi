// 渐显增强，fail-open 结构：
// 1) 无 JS / 减少动效偏好 / 脚本时序异常 → 内容直接可见，最坏是没有动画；
// 2) JS 只给"首屏之下"的元素挂 reveal-pending，滚动扫描逐个显形；
// 3) 用滚动扫描而非 IntersectionObserver：锚点瞬跳会掠过视口之间的区块，
//    observer 可能不触发，扫描对任何"进入或越过视口"的元素都生效。
document.documentElement.classList.add('js');

const reduced = matchMedia('(prefers-reduced-motion: reduce)').matches;
const targets = reduced ? [] : Array.from(document.querySelectorAll('.reveal, .stagger'));
const pending = [];

if (targets.length) {
  const edge = innerHeight * 0.92;
  for (const el of targets) {
    if (el.getBoundingClientRect().top > edge) {
      el.classList.add('reveal-pending');
      pending.push(el);
    }
  }
}

function sweep() {
  const limit = innerHeight * 0.92;
  for (let i = pending.length - 1; i >= 0; i--) {
    const el = pending[i];
    if (el.getBoundingClientRect().top < limit) {
      el.classList.add('is-visible');
      pending.splice(i, 1);
    }
  }
  if (pending.length === 0) {
    removeEventListener('scroll', sweep);
    removeEventListener('resize', sweep);
    removeEventListener('load', sweep);
  }
}

if (pending.length) {
  addEventListener('scroll', sweep, { passive: true });
  addEventListener('resize', sweep);
  addEventListener('load', sweep);
  requestAnimationFrame(sweep);
  setTimeout(sweep, 300);   // 后台标签页 rAF 可能被冻结，兜底
  setTimeout(sweep, 1200);
}
