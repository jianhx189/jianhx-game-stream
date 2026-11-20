(() => {
  'use strict';
  const payload = location.hash.slice(1);
  history.replaceState(null, '', location.pathname);
  const message = document.getElementById('message');
  const button = document.getElementById('launch');
  try {
    if (!/^[A-Za-z0-9_-]{1,1800}$/.test(payload)) throw new Error();
    const data = JSON.parse(atob(payload.replace(/-/g, '+').replace(/_/g, '/')));
    const parts = typeof data.ip === 'string' ? data.ip.split('.') : [];
    const validPort = n => Number.isInteger(n) && n >= 1 && n <= 65535;
    if (data.v !== 1 || !['lan', 'frp'].includes(data.mode) || parts.length !== 4 ||
        parts.some(n => !/^(0|[1-9]\d{0,2})$/.test(n) || +n > 255) ||
        !validPort(data.tcp) || !validPort(data.udp) || typeof data.key !== 'string' ||
        !/^[A-Za-z0-9_.@-]{1,64}$/.test(data.key)) throw new Error();
    message.textContent = '正在请求打开主控端，连接到 ' + data.ip + '。若浏览器未弹出提示，请点击下方按钮。';
    button.hidden = false;
    const open = () => location.assign('ministream://connect/#' + payload);
    button.addEventListener('click', open);
    open();
  } catch {
    message.textContent = '邀请缺失或格式有误。请从被控端重新生成并打开完整链接。';
  }
})();
