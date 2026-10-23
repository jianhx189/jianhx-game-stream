# MiniStream 官网

2026-10-23 更新。官网：https://ministream.jianhx.com/，Cloudflare Pages 项目 `jianhx-game-stream`。

## 本次更新

- 以 Sunshine / Moonlight + mslink 的实际架构重写产品介绍。
- 浅色、深绿与草绿色视觉；首页山景和向导是 CSS 绘制的示意，已明确标注。
- 两端口连接流程、Host / Client 下载入口、鼠标菜单说明、部署边界与 FAQ。
- 浏览器读取 GitHub 最新 Release 并匹配安装包；API 不可用时保留 Releases 页面入口。
- 手机导航、键盘焦点、减少动态效果偏好与 FAQ 交互。

## 构建和检查

执行 `node scripts/build-static.cjs` 生成 `dist/`。本地预览：`python -m http.server 8788 --bind 127.0.0.1 --directory dist`。

`scripts/verify.cjs` 使用 Playwright / Edge，检查 1440、768、390、320 像素宽度下无横向溢出、导航、FAQ、锚点和脚本错误。截图与检查结果位于 `previews/`。

通过 `scripts/deploy-cloudflare.py` 发布，仅上传 `dist/`。凭据通过隐藏 stdin 输入，不进入文件。使用 `scripts/verify-deployment.py` 核对线上文件。

## 内容维护

当前存在待修复的传输和认证问题，官网不声称延迟或弱网稳定性已经通过完整验收。上游归属与开源许可说明保留。当前发布标签与内容版本不一致时，以 GitHub 发布说明和文件摘要为准。
