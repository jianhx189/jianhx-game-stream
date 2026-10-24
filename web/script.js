(() => {
  'use strict';

  const menu = document.querySelector('.menu-toggle');
  const nav = document.querySelector('.site-nav');
  if (menu && nav) {
    const close = (restoreFocus = false) => {
      menu.setAttribute('aria-expanded', 'false');
      menu.setAttribute('aria-label', document.documentElement.lang.startsWith('en') ? 'Open navigation' : '展开导航');
      nav.classList.remove('open');
      if (restoreFocus) menu.focus();
    };

    menu.addEventListener('click', () => {
      const open = menu.getAttribute('aria-expanded') !== 'true';
      menu.setAttribute('aria-expanded', String(open));
      menu.setAttribute('aria-label', document.documentElement.lang.startsWith('en') ? (open ? 'Close navigation' : 'Open navigation') : (open ? '收起导航' : '展开导航'));
      nav.classList.toggle('open', open);
    });
    nav.querySelectorAll('a').forEach(link => link.addEventListener('click', () => close()));
    document.addEventListener('keydown', event => {
      if (event.key === 'Escape' && nav.classList.contains('open')) close(true);
    });
    document.addEventListener('click', event => {
      if (nav.classList.contains('open') && !nav.contains(event.target) && !menu.contains(event.target)) close();
    });
    window.addEventListener('resize', () => {
      if (window.innerWidth > 760) close();
    });
  }

  const info = document.getElementById('release-info');
  const links = Array.from(document.querySelectorAll('[data-file]'));
  if (!links.length) return;

  const english = document.documentElement.lang.startsWith('en');
  const tag = 'v2.2.0';
  const release = `https://github.com/jianhx189/jianhx-game-stream/releases/tag/${tag}`;
  const fallback = () => {
    links.forEach(link => {
      link.href = release;
      link.removeAttribute('aria-disabled');
    });
    if (info) info.textContent = english
      ? 'MiniStream 2.2 · target v2.2.0 · release manifest and SHA-256 on GitHub.'
      : 'MiniStream 2.2 · 下载目标 v2.2.0 · 发布清单与 SHA-256 以 GitHub Release 为准。';
  };

  fetch(new URL('release.json', document.baseURI))
    .then(response => response.ok ? response.json() : null)
    .then(data => {
      if (!data || data.tag_name !== tag || !Array.isArray(data.assets) || !data.assets.length) {
        fallback();
        return;
      }
      const assets = new Map(data.assets.map(asset => [asset.name, asset]));
      links.forEach(link => {
        const asset = assets.get(link.dataset.file);
        link.href = asset && typeof asset.browser_download_url === 'string'
          ? asset.browser_download_url
          : release;
        link.removeAttribute('aria-disabled');
      });
      if (info) {
        const date = (data.published_at || '').slice(0, 10);
        info.textContent = english
          ? `MiniStream 2.2 · ${tag}${date ? ` · ${date}` : ''} · verify SHA-256 in the release.`
          : `MiniStream 2.2 · ${tag}${date ? ` · ${date}` : ''} · 文件 SHA-256 见发布说明。`;
      }
    })
    .catch(fallback);
})();
