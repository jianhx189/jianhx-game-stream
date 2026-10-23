(() => {
  'use strict';

  /* 移动端菜单 */
  const m = document.querySelector('.menu-toggle'), n = document.querySelector('nav');
  if (m && n) {
    const close = () => { m.setAttribute('aria-expanded', 'false'); n.classList.remove('open'); };
    m.addEventListener('click', () => {
      const o = m.getAttribute('aria-expanded') !== 'true';
      m.setAttribute('aria-expanded', String(o));
      n.classList.toggle('open', o);
    });
    n.querySelectorAll('a').forEach(a => a.addEventListener('click', close));
    document.addEventListener('keydown', e => { if (e.key === 'Escape') close(); });
  }

  /* 下载按钮：从最新 Release 的资产列表里取真实下载地址 */
  const info = document.getElementById('release-info');
  const links = Array.from(document.querySelectorAll('[data-file]'));
  const RELEASES = 'https://github.com/jianhx189/jianhx-game-stream/releases/latest';

  if (!links.length) return;

  const fallback = () => {
    links.forEach(a => {
      a.href = RELEASES;
      a.removeAttribute('aria-disabled');
    });
    if (info) info.textContent = '最新发布、文件校验值与更新说明见 GitHub Releases。';
  };

  fetch('https://api.github.com/repos/jianhx189/jianhx-game-stream/releases/latest', {
    headers: { 'Accept': 'application/vnd.github+json' }
  })
    .then(r => (r.ok ? r.json() : null))
    .then(rel => {
      if (!rel || !rel.assets || !rel.assets.length) { fallback(); return; }
      const byName = {};
      rel.assets.forEach(a => { byName[a.name] = a; });

      links.forEach(a => {
        const name = a.getAttribute('data-file');
        const asset = byName[name];
        if (asset) {
          a.href = asset.browser_download_url;
          a.removeAttribute('aria-disabled');
        } else {
          a.href = RELEASES;
          a.removeAttribute('aria-disabled');
        }
      });

      if (info) {
        const date = (rel.published_at || '').slice(0, 10);
        info.textContent = '最新版本 ' + (rel.tag_name || '') +
          (date ? '，发布于 ' + date : '') + '。文件 SHA-256 见发布说明。';
      }
    })
    .catch(fallback);
})();
