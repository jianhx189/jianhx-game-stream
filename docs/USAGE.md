# 使用与升级 / User guide

## 两端角色与安装

Host 安装在实际运行游戏/桌面的 Windows x64 主机，Client 安装在操作电脑。获取同一正式 Release 的两份安装包，按 SHA256SUMS 校验。程序携带匹配的 VC143 运行库；安装器检测并按需运行微软 x64 redist，失败时不继续。当前仍需更多干净 Win10/Win11 环境验证。

被控端安装时设置自己的隧道口令与端口，保存安装信息。打开桌面「MiniStream 被控端」查看控制台。主控端向导填写主机地址、TCP/UDP 端口和隧道口令。Sunshine 管理密码为可选自动配对信息；没有时使用手动 PIN。

## 网络

同一局域网先填被控端 LAN IPv4。跨网配置两条独立映射：公网 TCP → 本机 TCP 14738，公网 UDP → 本机 UDP 14739（实际端口以配置为准）。公网端口可以与本机不同，客户端必须填写公网实际值。示例使用 `host.example.com`，不是公共服务。

不要把 Sunshine 管理页面作为日常公网串流入口。自定义隧道不替代加密 VPN，请阅读 SECURITY。FRP 的 UDP 类型也可能由其他底层传输承载，实际延迟需实测。

## 连接与指标

测试连接后确认已配对，再打开应用或桌面。隧道连接不代表媒体已开始。控制台每秒显示实际后台上报；旧后台、错误根目录或过期采样不会显示伪造数据。媒体指标需要新版主控端正在串流且持续上报。

RTT 为隧道心跳往返，不是端到端鼠标延迟。网络丢帧为媒体层最终未恢复的帧，不是 IP 丢包率。显示 TCP 兼容通道时媒体仍可能可用，但可能受队头阻塞影响。UDP 字节数包含逻辑媒体搬运，不能单独证明物理 UDP 可用。

串流中 `F12` 或 `Ctrl+Alt+Shift+X` 打开菜单。游戏通常使用相对鼠标，桌面按需使用绝对鼠标。遇到输入异常先记录焦点、菜单、模式、画面/声音状态和时间。

## 升级与回退

结束串流后保存自己的配置与安装信息。Host 在已有目录覆盖升级，安装器会尝试识别旧 x86/x64 路径并保留隧道配置、Sunshine 配对与账号。不要只打开新 GUI 而继续运行旧 mslink；用状态文件 PID 对照实际后台。旧 Client 配置可能需要在新版向导重新填写。

升级后核对两端版本、实际 EXE 路径、状态更新时间、认证设备和传输模式，再启动媒体。回退前同样结束连接并保留配置，避免不同版本组件混装。

## English quick start

Install Host on the PC running your game, and Client on the PC you operate. Use matching releases and verify SHA-256. On LAN enter the host's LAN IP; with FRP configure separate public TCP and UDP mappings and enter the real public ports. Keep your credentials private, complete pairing and launch a desktop/application. Telemetry appears only when the corresponding background process and media reports are current. End sessions before upgrading over the existing host directory.
