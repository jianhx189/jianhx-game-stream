# MiniStream 官网

MiniStream 2.2 的静态官网，使用 HTML、CSS 与原生 JavaScript，主站为 <https://ministream.jianhx.com/>。设计语言以深空石墨 `#0b1020`、青蓝 `#66c9f2` 和少量紫 `#a899ff` 为主，支持键盘导航、移动端布局与 `prefers-reduced-motion`。

## 页面

- `index.html`：中文首页、分端工作台、真实程序截图、连接架构、下载与验证范围。
- `en.html`：与首页信息对应的英文产品页。
- `tutorial.html`：安装、局域网/跨网连接、遥测指标解释、弱网排障与升级边界。
- `changelog.html`：2.2 更新、2.1.4 修复背景、下载校验与验证范围。
- `styles.css` / `script.js`：全站共享样式、移动导航和下载资产选择。
- `release.json` / `updates.json`：站点发布元数据；静态构建会同步，但发布生成流程不由官网构建脚本负责。

当前下载目标为 `v2.2.0`，页面版本文案为 MiniStream 2.2。`script.js` 只从 `tag_name` 精确匹配 `v2.2.0` 的 `release.json` 中读取安装包直链；元数据缺失、请求失败或版本不匹配时统一回退到该 Release 标签页，避免把旧版资产误标为 2.2。

## 截图

沿用 `assets/client-home.jpg`、`assets/host-overview.jpg` 和 `assets/client-setup.jpg`，等待新版真实程序截图替换，文件名保持不变。截图说明统一为“新版真实程序离线或测试状态，不表示真实游戏性能”，不得作为帧率、延迟或兼容性承诺。`brand.svg` 为原创品牌图标。

## 构建

```bash
node web/scripts/build-static.cjs
```

输出目录为 `web/dist`，供 Cloudflare Pages 与 GitHub Pages 使用。构建会先清理该输出目录，再同步以下明确允许的站点文件：

- 四个 HTML 页面、共享 CSS 和 JavaScript；
- `release.json`、`updates.json`、`robots.txt`、`sitemap.xml`；
- `assets/` 顶层中扩展名匹配的公开图片、图标与字体（SVG、JPG/JPEG、PNG、WebP、GIF、ICO、WOFF/WOFF2）。

脚本不使用递归整目录复制，不同步 `web/dist` 自身、源码、文档、私人配置、凭据、日志、安装包或发布工具输出。新增站点文件或资源类型时，需要同时更新允许清单，不能改成全量复制。

## 本地检查

不安装第三方依赖。使用 Python 标准库 `html.parser.HTMLParser` 检查四个 HTML 文件的标签配对、重复 ID、锚点、图片替代文本和本地资源引用，并用 `node --check web/script.js` 检查脚本语法。发布前还应实际检查桌面与移动端导航、键盘焦点、减少动态偏好及真实下载资产。

验证文案只陈述已执行范围：本机 CTest 7/7、7 类弱网场景 50/50 检查通过；完整干净 Windows 虚拟机安装，以及没有 ISO 或管理员权限的限制环境尚未完成。不能宣称 Windows 10/11 全部环境、所有 GPU/驱动组合或所有游戏均已测试。
