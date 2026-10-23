# 崩溃转储分析工具（MiniStream）

程序闪退后，**先看崩溃日志，再需要细节时看转储**。

## 一、程序自己留下的东西

| 文件 | 位置 | 内容 |
|------|------|------|
| `crash.log` | 被控端/远控端 exe 同目录 | 异常类型（如 `std::system_error`）、`what()` 文本、调用栈（模块+偏移） |
| `crash_host_<pid>.dmp` / `crash_client_<pid>.dmp` | 同上 | 标准 minidump（Windows 通用格式） |
| `host.log` / `client.log` | 同上 | 正常运行日志（UTF-8 带 BOM，记事本可直接看） |

退出码含义：`3` = 未捕获 C++ 异常，`4` = 访问越界等结构化异常，`5` = CRT 非法参数，
`6` = 拒绝启动（已经有一个被控端在运行）。

## 二、怎么看

### 1. 最快：`crash.log`

最后一段通常已经把原因写清楚了，例如：

```
[02:04:46.896][host] ===== 未捕获的 C++ 异常（std::terminate）=====
[02:04:46.896][host] 类型: std::system_error  what(): ...  code: 35 (resource deadlock would occur)
[02:04:46.896][host] ---- 调用栈（模块+偏移）----
[02:04:46.896][host]   #00 ministream-host.exe+0x2A7D3
```

### 2. 精确调用栈：minidump

需要 Python 3（任意版本，只用标准库）：

```
python minidump_info.py crash_host_12345.dmp
```

输出：异常码、fast-fail 子码、崩溃线程的 RIP/RSP、栈上所有指向已知模块的返回地址、
以及所有线程当时停在哪里。

要翻译成**函数名 + 源码行号**，需要有符号的构建：

```
python minidump_info.py crash_host_12345.dmp ^
    --exe ..\build\map\bin\ministream-host.exe ^
    --map ..\build\map\bin\ministream-host.map
```

`build-map.bat` 生成的 `.map` 与发布版 exe 只差 PE 头 4 个字节（时间戳/校验），
所以它对发布版同样有效——这是本项目定位"黑屏后闪退"的关键手段。

### 3. 栈上所有代码地址：`stack_code_frames.py`

当调用栈被异常展开（`RtlUnwind`）过、返回地址看起来缺失时，用它把崩溃线程栈上
**所有落在代码区的地址**列出来，再逐个核对符号：

```
python stack_code_frames.py crash_host_12345.dmp ministream-host.map 4096
```

## 三、卡死（不崩溃但没反应）怎么查

闪退有 `.dmp`，**卡死没有**——这时要主动抓一份转储再看每个线程在哪。三个脚本配合用：

### 4. 抓卡死现场：任务管理器 → 右键进程 → "创建转储文件"
或者用管理员 PowerShell 调 dbghelp 的 `MiniDumpWriteDump`（`DumpType=2` 全内存）。

### 5. 每个线程卡在哪：`hang_threads.py`

```
python hang_threads.py hang.dmp ministream-client.exe ministream-client.map
```

对每个线程扫描栈，挑出落在本程序里的返回地址并用 `.map` 精确符号化，
能直接看出"主线程卡在 renderer.present / 控制线程卡在 recv_frame"这种结论。

### 6. 连系统 DLL 的帧也要名字：`dump_threads_detail.py` + `pe_exports_from_dump.py`

前者打印所有线程的 RIP 与栈上各帧（模块+偏移）；后者**不需要符号服务器**，
直接解析转储里已加载 DLL 的导出表，把偏移翻译成 `ntdll!ZwWaitForSingleObject`
这种名字。判断"卡在锁上（RtlpWaitForCriticalSection）还是卡在网络
（NtDeviceIoControlFile）"就靠它：

```
python dump_threads_detail.py hang.dmp ministream-client.exe ministream-client.map
python pe_exports_from_dump.py hang.dmp 52264,60260 30
```

## 四、注意事项

- minidump 里可能包含屏幕内容/内存片段，**别外传**。
- 转储只在崩溃时生成；正常运行不会有 `.dmp` 文件。
- 如果 `crash.log` 都不存在，说明程序不是崩溃退出（例如被任务管理器结束），
  这时看 `host.log`/`client.log` 的最后几行。

## 五、真机功能验证（光标 / 传输自愈）

这两块都"看起来像好了、实际可能没生效"，所以有专门的自动化脚本，
避免每次都让人肉眼看：

| 脚本 | 作用 |
|------|------|
| `ssh_cursor_transport_test.py` | 经 FRP 连客户机 VM 上的真实客户端：验证远端光标位置/形状/隐藏、TCP→UDP 与 UDP→TCP 自愈 |
| `ssh_mouse_mapping_test.py` | 验证**鼠标绝对映射**：在客户机交互桌面把光标移到指定位置，读被控端真实光标位置，误差应为 0；并验证进游戏/回菜单时自动切相对/绝对 |
| `ssh_ui_test.py` | 真机 UI 测试：小悬浮按钮点击 → 工具栏能否**反复**弹出、画质面板展开、自动收起；`--toast` 验证切 TCP/UDP 只剩顶部小胶囊 |
| `local_e2e_cursor.ps1` | 本机起一套被控端+远控端做冒烟测试（注意：同一台机器上客户端捕获会 `ShowCursor(FALSE)`，被控端看到的光标也会被藏起来，所以光标"可见"路径只能真机验证） |
| `ssh_screenshot.py` / `ssh_refshot.py` | 客户机遇真实交互桌面截图（计划任务方式，SSH 里直接截会黑屏） |

用法：

```
python tools/ssh_cursor_transport_test.py            # 光标：位置/形状/隐藏
python tools/ssh_cursor_transport_test.py --tcp      # 再加：开局切 TCP -> 自动切回 UDP
python tools/ssh_cursor_transport_test.py --dropudp  # 再加：切断 UDP -> 切 TCP -> 恢复 -> 切回 UDP
```

脚本靠被控端的**自测钩子**制造确定性条件（正常运行时这些环境变量不存在，零开销）：

| 环境变量 | 含义 |
|----------|------|
| `MINISTREAM_TEST_CURSOR_FLAG=<路径>` | 该文件存在时，被控端强制上报"光标不可见"（等价游戏进内场 `ShowCursor(FALSE)`） |
| `MINISTREAM_TEST_DROP_UDP_FLAG=<路径>` | 该文件存在时，被控端不发 UDP 视频（模拟 UDP 路径不通） |
| `MINISTREAM_TEST_TCP_START=1`（远控端） | 开局先切 TCP，用来验证能否自动探测回 UDP |
| `MINISTREAM_FORCE_TCP_VIDEO=1`（远控端） | 钉死 TCP，不探测回 UDP（排障用） |
| `MINISTREAM_TEST_DISABLE_INPUT=1`（被控端） | 禁用输入注入（真机 UI 自动化点击画面时，避免点到本机桌面） |
| `MINISTREAM_TEST_TOAST_MS=<毫秒>`（远控端） | 拉长顶部小提示条的显示时长（截图脚本启动慢时用） |
| `MINISTREAM_TEST_CURSOR_HIDE` 系列 | 见 `ssh_cursor_transport_test.py` 注释 |
| `MINISTREAM_TEST_LOSS_PCT=<0~100>`（被控端） | **模拟 UDP 链路丢包**（数据片和校验包一视同仁），用来在回环上验证 FEC 与"等 IDR"；正常使用不设 |

### 回环丢包验证（FEC / 等 IDR / 传输滞回）

不需要两台机器，也不需要真的丢包，一条命令就能把抗丢包链路跑起来：

```powershell
# 默认按 5% 丢包跑 18 秒；被控端在本机 127.0.0.1:14738
powershell -NoProfile -ExecutionPolicy Bypass -File tools\local_fec_test.ps1 -LossPct 5 -Seconds 18

# 0% 基线（看正常画质/延迟）；15% 重丢包（看是否会切 TCP 并稳定保持）
powershell -NoProfile -ExecutionPolicy Bypass -File tools\local_fec_test.ps1 -LossPct 0
powershell -NoProfile -ExecutionPolicy Bypass -File tools\local_fec_test.ps1 -LossPct 15
```

脚本会把两端日志里的关键行打出来，重点看客户端的
`[串流] ... 丢帧 x% 分片丢 y% | FEC 校验包 n 恢复 m 片 | NACK k 救回 j 帧 | ... 排队 z ms`
和 `[延迟] p50/p90/p99 | jank 小/中/大`：

| 现象 | 含义 |
|------|------|
| `分片丢` 明显高于 `-LossPct` | 统计/重组有问题（迟到包被误判成丢帧等） |
| `FEC 校验包` 增长但 `恢复` 一直 0 | 校验包没被用在恢复上（组下标/末组长度算错） |
| `NACK` 增长但 `救回` 一直 0 | 重传请求没被发端响应（帧历史/序号算错，或重传被限速挡住） |
| 5% 丢包下 `丢帧` 仍 >1% | NACK 没起作用（检查被控端"NACK 重传"日志窗口） |
| 频繁 `帧号不连续 ... 等 IDR` 但画面不花 | 正常：丢包救不回来时宁可冻结也不花屏 |
| 5% 以内一直停在 UDP 不切 TCP | 正常：FEC + 降低码率应该扛得住 |
| 15% 丢包下切一次 TCP 后不再来回切 | 正常：传输滞回生效 |
