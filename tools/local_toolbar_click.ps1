# 本地复现：工具栏按钮点击是否生效（真实鼠标事件，不做 PostMessage 作弊）
# 用途：定位「工具栏显示正常但按钮点不动」的问题。
$ErrorActionPreference = 'Stop'
$bin = 'D:\远程项目\build\msvc\bin'
$logDir = 'D:\远程项目\build\client-side'
New-Item -ItemType Directory -Force -Path $logDir | Out-Null

Add-Type @'
using System;
using System.Runtime.InteropServices;
using System.Text;
public class W {
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint dx, uint dy, uint d, IntPtr e);
  [DllImport("user32.dll")] public static extern IntPtr FindWindowW(string cls, string name);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr lp);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  public delegate bool EnumProc(IntPtr h, IntPtr lp);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int left, top, right, bottom; }
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int x, y; }
}
'@

function Kill-Test {
  Get-Process ministream-client, ministream-host -ErrorAction SilentlyContinue |
    Where-Object { $_.Path -like '*\build\msvc\bin\*' } | Stop-Process -Force
}
Kill-Test
Start-Sleep -Milliseconds 400

$hostLog = Join-Path $bin 'host_clicktest.log'
$cliLog  = Join-Path $bin 'client.log'
foreach ($f in @($hostLog, $cliLog)) { if (Test-Path $f) { Remove-Item $f -Force } }

# 被控端：14740 端口，禁用键鼠注入（避免点击打到真实桌面）
$env:MINISTREAM_TEST_DISABLE_INPUT = '1'
$hostP = Start-Process -FilePath (Join-Path $bin 'ministream-host.exe') `
  -ArgumentList '--port','14740','--password','123','--log-level','1' `
  -WorkingDirectory $bin -PassThru -WindowStyle Hidden
Start-Sleep -Seconds 2

# 远控端：窗口化 + 绝对鼠标模式 + 调试日志
$env:MINISTREAM_MOUSE_MODE = 'absolute'
$cliP = Start-Process -FilePath (Join-Path $bin 'ministream-client.exe') `
  -ArgumentList '--auto','--host','127.0.0.1','--port','14740','--password','123',
                '--fullscreen','0','--log-level','1','--seconds','70' `
  -WorkingDirectory $bin -PassThru

# 等窗口 + 首帧
$hwnd = [IntPtr]::Zero
for ($i = 0; $i -lt 100; $i++) {
  $hwnd = [W]::FindWindowW('MiniStreamRenderWnd', $null)
  if ($hwnd -ne [IntPtr]::Zero) { break }
  Start-Sleep -Milliseconds 200
}
if ($hwnd -eq [IntPtr]::Zero) { Write-Host 'FAIL: 渲染窗口没建出来'; Kill-Test; exit 1 }
Start-Sleep -Seconds 4

$cr = New-Object W+RECT
[void][W]::GetClientRect($hwnd, [ref]$cr)
$origin = New-Object W+POINT
$origin.x = 0; $origin.y = 0
[void][W]::ClientToScreen($hwnd, [ref]$origin)
$cw = $cr.right - $cr.left
Write-Host ("client area: {0}x{1}, origin=({2},{3})" -f $cw, ($cr.bottom - $cr.top), $origin.x, $origin.y)

[void][W]::SetForegroundWindow($hwnd)
Start-Sleep -Milliseconds 300

# ---- 计算坐标（与 session_toolbar.cpp 的 rebuild_layout 一致）----
$rightW = 92*3 + 112 + 92 + 104 + 12*2 + 26
$statusW = $cw - $rightW - 24
if ($statusW -lt 240) { $statusW = 240 }
$chevronX = 12 + $statusW + 12 + 1 + 12 + 46   # 按钮中心
$dockX = [int](($cw - 64) / 2 + 32)
$dockY = 5 + 11
$chevronY = (44 - 24) / 2 + 12

function Click-At([int]$x, [int]$y) {
  [void][W]::SetCursorPos($origin.x + $x, $origin.y + $y)
  Start-Sleep -Milliseconds 200
  [W]::mouse_event(0x0002, 0, 0, 0, [IntPtr]::Zero)
  Start-Sleep -Milliseconds 60
  [W]::mouse_event(0x0004, 0, 0, 0, [IntPtr]::Zero)
  Start-Sleep -Milliseconds 120
}
function Move-At([int]$x, [int]$y) {
  [void][W]::SetCursorPos($origin.x + $x, $origin.y + $y)
}

Write-Host ("dock=({0},{1}) chevron=({2},{3})" -f $dockX, $dockY, $chevronX, $chevronY)

# ---- 1. 点小悬浮按钮唤出工具栏 ----
Move-At $dockX $dockY
Start-Sleep -Milliseconds 300
Click-At $dockX $dockY
Start-Sleep -Milliseconds 900
$shot = Join-Path $logDir 'local_tb_1_dock.png'
Add-Type -AssemblyName System.Drawing
$bmp = New-Object System.Drawing.Bitmap 1280, 200
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($origin.x, $origin.y, 0, 0, (New-Object System.Drawing.Size 1280, 200))
$bmp.Save($shot, [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose(); $bmp.Dispose()

# ---- 2. 点「画质 ▾」 ----
Click-At $chevronX $chevronY
Start-Sleep -Milliseconds 900
$shot2 = Join-Path $logDir 'local_tb_2_chevron.png'
$bmp = New-Object System.Drawing.Bitmap 1280, 400
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($origin.x, $origin.y, 0, 0, (New-Object System.Drawing.Size 1280, 400))
$bmp.Save($shot2, [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose(); $bmp.Dispose()

# ---- 3. 再点一次收起，再点全屏按钮 ----
Click-At $chevronX $chevronY
Start-Sleep -Milliseconds 400
$fsX = $chevronX + 92 + 8 + 46
Click-At $fsX $chevronY
Start-Sleep -Milliseconds 900

Write-Host ''
Write-Host '===== client.log 关键行 ====='
Select-String -Path $cliLog -Pattern '工具栏|画质面板|小悬浮按钮|鼠标模式|全屏|窗口' -ErrorAction SilentlyContinue |
  ForEach-Object { $_.Line }
Write-Host ''
Write-Host '===== 截图 ====='
Get-Item $shot, $shot2 | ForEach-Object { Write-Host $_.FullName $_.Length }

Kill-Test
