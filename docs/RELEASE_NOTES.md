# MiniStream 2.3.1

- 被控端重构连接设置页。局域网与 FRP 独立切换，邀请和代理复制操作固定在底部。
- 勾选自定义映射后，可独立设置 TCP、UDP 的公网端口和本机隧道端口。内网端口与监听配置不一致时，界面会引导应用更改。
- 复制项仅包含两条 frpc 代理配置。frps 由用户自行管理，被控端生成邀请时不连接 frps，也不发起公网探测。请从外网主控端验证 TCP 与 UDP。

到主机下载 Host，到操作电脑下载 Client。结束串流后在原目录覆盖升级。请对照本版本的 SHA-256 校验安装文件。

## English

The Host connection settings have separate LAN and FRP views, a fixed invitation bar and separate public-to-local TCP and UDP port mappings. Copy the proxy definitions into your existing frpc configuration. Manage frps independently and test public connectivity from the client.

[使用教程](https://ministream.jianhx.com/tutorial.html) · [English overview](https://ministream.jianhx.com/en.html)
