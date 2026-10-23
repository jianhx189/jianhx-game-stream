> 版本说明：本文记录正式发布前的本机热修复过程。2.1.2 正式版已经纳入该修复；文末 B957… 哈希属于当晚临时热修二进制，正式包请核对 release-manifest.json 和 SHA256SUMS.txt。

# UDP 映射正常却持续回退 TCP：多网卡回包源地址修复

## 已确认的原因

2026-10-23 对用户真实连接进行只读、限时抓取后发现：UDP HELLO 到达被控端的网卡地址 A，后台从另一网卡地址 B 发出了 HELLO_ACK。修复前 10 秒观察到 20 个 HELLO 和 20 个源地址错误的 ACK。客户端的有效 ACK 始终为 0。

被控端使用 `bind(0.0.0.0:端口)` 接收数据，再用普通 `sendto()` 回包。Windows 在存在多个同网段网卡、Hyper-V 或虚拟网卡时，可能按到对端的路由选择另一源地址。对严格检查 UDP 对端地址的转发端，这类应答不属于原有目标端点，因此无法正常返回客户端。

本机之前的临时映射测试显式使用入包的目标地址发送回包，所以映射测试成功与应用失败并不矛盾。

原始客户端诊断的 `udp_probe_sent=156`、`udp_probe_ack=0`、`udp_data_received=0`，只能证明没有收到有效的应用层 UDP 应答；`udp_auth_rejected=0` 不能证明 UDP 包未到达后台。`udp_bytes_down` 包含通过 TCP 兼容通道搬运的逻辑 UDP 数据，不能把其数值当作物理 UDP 通行证据。

## 已实现的修改

实现位于 `源码/mslink/src/relay.cpp`，未修改线上协议或客户端配置格式。

1. 在被控端 UDP 监听套接字上启用 `IP_PKTINFO`，通过 `WSAIoctl` 获取 `WSARecvMsg` / `WSASendMsg` 扩展。
2. 用 `WSARecvMsg` 收包，读取 `IN_PKTINFO.ipi_addr` 和 `ipi_ifindex`，保留入包目标地址与入网卡。
3. 只有在会话、认证标签和序号校验通过后，才更新当前客户端的回包路径。未认证数据不能改变回包地址。
4. HELLO_ACK 和所有被控端到主控端的 UDP DATA 都使用 `WSASendMsg` 的 `IP_PKTINFO` 控制信息，固定到相同源地址、源端口及接口。
5. 路径信息缺失或固定路径发送失败时，记录错误，不静默退回可能选错源地址的普通 `sendto()`。
6. 会话关闭时清除缓存路径。新增 `udp_hello_received`、`udp_hello_ack_sent`、`udp_session_rejected`、`udp_reply_errors`、`udp_reply_route_pinned` 诊断字段；不导出会话密钥。

没有写死用户网卡地址，也没有修改全局路由、关闭代理、关闭防火墙或重置 Sunshine 配置。后台继续监听全部接口。

微软接口说明：[IN_PKTINFO](https://learn.microsoft.com/en-us/windows/win32/api/ws2ipdef/ns-ws2ipdef-in_pktinfo)、[WSASendMsg](https://learn.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-wsasendmsg)。

## 验证结果与范围

- 常规隧道自检通过：认证、TCP 往返、3 路并发 TCP、UDP 往返、100/100 UDP 突发、防重放窗口、认证遥测。
- CMake 构建通过，CTest 的 `udp_reply_source` 与 `relay_selftest` 两项全部通过。
- 新增 `tests/udp_packet_info_test.cpp`：使用 connected UDP 套接字严格限制回包来源，验证源地址保持与缺失路径拒绝。回环及两个真实本机网卡地址分别 20/20 通过，共 60/60。
- 同机跨网卡的额外模拟在接收请求前即超时；未把该模拟计入通过项。真实穿透连接用于最终多网卡验收。
- 本机后台已替换为修复版；配置哈希核对保持不变，并保留可回滚的旧可执行文件。
- 修复后真实连接的 10 秒抓取中，4 个 HELLO 对应 4 个 ACK，ACK 已使用入包的目标网卡地址作为源地址；未再看到错误源地址 ACK。
- 客户端 `transport` 已从 `tcp_fallback` 变为 `udp`，有效 ACK 计数持续增长。后台 `udp_reply_route_pinned=true`、`udp_reply_errors=0`。
- 验收时媒体上报已过期，尚未验证一次新的桌面/游戏媒体串流及持续画质表现。UDP 探测恢复不代表已完成所有卡顿问题的验收。
- 结束检查时后台仍正常监听，当前已无活动隧道；不能仅凭该状态判断远端退出还是其他断开原因。重新连接后可继续采集媒体指标。

## 构建与回归

```powershell
cmake -S 源码/mslink -B 构建产物/mslink-regression -DBUILD_TESTING=ON
cmake --build 构建产物/mslink-regression --config Release
ctest --test-dir 构建产物/mslink-regression -C Release --output-on-failure
```

也可给测试程序传入 `<目标本机地址> <客户端绑定地址>` 参数对；地址必须实际存在且路由可达。默认 CTest 使用回环地址，不依赖某个用户的私有网络。

当前本机修复版：`构建产物/udp-source-fix/mslink.exe`。SHA256：`B957D1630764F222B2B0FAF43219EADD37012A7953EBDDAACA93D866F02937CF`。

本次改动首先修复并验证了本机后台；此前已发布的 v2.1.1-preview 安装包未随本次本机热修复自动更新，重新发布前应重建安装包及校验文件。
