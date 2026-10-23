# MiniStream 官网

静态 HTML / CSS / JavaScript，主站为 https://ministream.jianhx.com/ 。

- `index.html`：产品介绍，真实界面截图，正式版与 UI 预览版入口。
- `tutorial.html`：安装、连接、局域网/跨网与故障排查。
- `assets/brand.svg`：原创品牌图标；三张 JPG 为本次编译程序的真实运行窗口，未包含私人连接信息。

运行 `node web/scripts/build-static.cjs` 生成 `web/dist`，该目录供 Cloudflare Pages 与 GitHub Pages 发布。不要复制凭据、配置、安装包、日志或发布工具输出到站点。

截图对应 2.1.0 UI 预览版，不表示跨网游戏、Win10 或服务管理已全面验收。发布说明和下载校验值以 GitHub Release 为准。
