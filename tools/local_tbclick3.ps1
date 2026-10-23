# 本地验证：画质面板防双击 / 收起按钮 / 点画面关闭 / 锐化
$ErrorActionPreference = 'Stop'
$bin = 'D:\远程项目\build\msvc\bin'
$logDir = 'D:\远程项目\build\client-side'
New-Item -ItemType Directory -Force -Path $logDir | Out-Null

Add-Type @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public class W {
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint dx, uint dy, uint d, IntPtr e);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr lp);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
  public delegate bool EnumProc(IntPtr h, IntPtr lp);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int left, top, right, bottom; }
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int x, y; }
  public static IntPtr found = IntPtr.Zero;
  public static uint wantPid = 0;
  public static bool Cb(IntPtr h, IntPtr lp) {
    var sb = new StringBuilder(128);
    GetClassNameW(h, sb, 128);
    if (sb.ToString() == "MiniStreamRenderWnd") {
      uint pid; GetWindowThreadProcessId(h, out pid);
      if (wantPid == 0 || pid == wantPid) { found = h; return false; }
    }
    return true;
  }
  public static IntPtr FindByPid(uint pid) {
    wantPid = pid; found = IntPtr.Zero;
    EnumWindows(Cb, IntPtr.Zero);
    return found;
  }
}
'@

function Kill-Test {
  Get-CimInstance Win32_Process -Filter "Name='ministream-client.exe'" -ErrorAction SilentlyContinue |
    Where-Object { $_.ExecutablePath -like '*\build\msvc\bin\*' } |
    ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
}
Kill-Test
Start-Sleep -Milliseconds 300

$cliLog = Join-Path $bin 'client.log'
if (Test-Path $cliLog) { Remove-Item $cliLog -Force }

$env:MINISTREAM_NO_INPUT = '1'
$env:MINISTREAM_MOUSE_MODE = 'absolute'
$env:MINISTREAM_SHARPEN = '2'
$cliP = Start-Process -FilePath (Join-Path $bin 'ministream-client.exe') `
  -ArgumentList '--auto','--host','127.0.0.1','--port','14738','--password','123',
                '--fullscreen','0','--log-level','1','--seconds','70' `
  -WorkingDirectory $bin -PassThru

$hwnd = [IntPtr]::Zero
for ($i = 0; $i -lt 150; $i++) {
  $hwnd = [W]::FindByPid($cliP.Id)
  if ($hwnd -ne [IntPtr]::Zero) { break }
  Start-Sleep -Milliseconds 200
}
if ($hwnd -eq [IntPtr]::Zero) {
  Write-Host 'FAIL: 渲染窗口没建出来'
  if (Test-Path $cliLog) { Get-Content $cliLog -Tail 20 }
  Kill-Test
  exit 1
}
Start-Sleep -Seconds 5

$cr = New-Object W+RECT
[void][W]::GetClientRect($hwnd, [ref]$cr)
$origin = New-Object W+POINT
[void][W]::ClientToScreen($hwnd, [ref]$origin)
$cw = $cr.right - $cr.left
[void][W]::SetForegroundWindow($hwnd)
Start-Sleep -Milliseconds 400

$rightW = 92*3 + 112 + 92 + 104 + 12*2 + 26
$statusW = $cw - $rightW - 24
if ($statusW -lt 240) { $statusW = 240 }
$chevronX = 12 + $statusW + 12 + 1 + 12 + 46
$chevronY = (44 - 24) / 2 + 12
$closeX = 492 + 26      # 面板右上「收起」中心
$closeY = 44 + 10 + 11

function Move-At([int]$x, [int]$y) { [void][W]::SetCursorPos($origin.x + $x, $origin.y + $y) }
function Click-At([int]$x, [int]$y, [int]$gap = 200) {
  Move-At $x $y
  Start-Sleep -Milliseconds $gap
  [W]::mouse_event(0x0002, 0, 0, 0, [IntPtr]::Zero)
  Start-Sleep -Milliseconds 70
  [W]::mouse_event(0x0004, 0, 0, 0, [IntPtr]::Zero)
  Start-Sleep -Milliseconds 150
}
function Shot([string]$name, [int]$w, [int]$h) {
  Add-Type -AssemblyName System.Drawing
  $bmp = New-Object System.Drawing.Bitmap $w, $h
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $g.CopyFromScreen($origin.x, $origin.y, 0, 0, (New-Object System.Drawing.Size $w, $h))
  $p = Join-Path $logDir $name
  $bmp.Save($p, [System.Drawing.Imaging.ImageFormat]::Png)
  $g.Dispose(); $bmp.Dispose()
  Write-Host "[shot] $p"
}

Write-Host ("chevron=($chevronX,$chevronY) close=($closeX,$closeY)")

# 1) 顶边唤出工具栏
Move-At 960 1
Start-Sleep -Milliseconds 700

# 2) 单点「画质」-> 面板展开
Click-At $chevronX $chevronY 300
Start-Sleep -Milliseconds 700
Shot 'v2_1_panel.png' 1280 400

# 3) 双击「画质」（两下间隔 120ms）-> 面板必须保持展开
Click-At $chevronX $chevronY 200
Click-At $chevronX $chevronY 120
Start-Sleep -Milliseconds 700
Shot 'v2_2_after_doubleclick.png' 1280 400

# 4) 点画面 -> 面板关闭
Click-At 900 600 200
Start-Sleep -Milliseconds 600
Shot 'v2_3_after_videoclick.png' 1280 400

# 5) 再点「画质」-> 打开，再点右上「收起」-> 关闭
Click-At $chevronX $chevronY 300
Start-Sleep -Milliseconds 600
Shot 'v2_4_reopen.png' 1280 400
Click-At $closeX $closeY 250
Start-Sleep -Milliseconds 700
Shot 'v2_5_closed.png' 1280 400

Write-Host ''
Write-Host '===== client.log 关键行 ====='
if (Test-Path $cliLog) {
  Select-String -Path $cliLog -Pattern '工具栏|画质面板|工具栏按钮|锐化|鼠标模式' |
    ForEach-Object { $_.Line }
}

Kill-Test
