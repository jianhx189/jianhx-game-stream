# =============================================================================
#  local_e2e_cursor.ps1 - 鏈満璺戜竴娆?琚帶绔?+ 杩滄帶绔?绔埌绔紝楠岃瘉鍏夋爣閾捐矾
#
#  楠岃瘉鐐癸細
#   1. 琚帶绔兘鍙栧埌妗岄潰鍏夋爣褰㈢姸骞跺彂閫侊紙host.log锛?#   2. 杩滄帶绔兘鏀跺埌鍏夋爣褰㈢姸/浣嶇疆锛坈lient.log锛?#   3. 鐢婚潰閲岀湡鐨勭敾鍑轰簡鍏夋爣锛堟埅鍥?+ 鍍忕礌瀵规瘮锛屼笉闇€瑕佷汉鐪嬶級
#   4. 琚帶绔妸绯荤粺鍏夋爣钘忚捣鏉ユ椂锛岃繙鎺х璺熺潃闅愯棌锛堟ā鎷熸父鎴忓唴鍦猴級
#
#  鐢ㄦ硶锛歱owershell -ExecutionPolicy Bypass -File tools\local_e2e_cursor.ps1
# =============================================================================
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$bin  = Join-Path $root 'build\msvc\bin'
$hostExe   = Join-Path $bin 'ministream-host.exe'
$clientExe = Join-Path $bin 'ministream-client.exe'
$outDir    = Join-Path $root 'build\cursor_e2e'
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
Add-Type -Namespace Win -Name Native -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
[DllImport("user32.dll")] public static extern bool GetCursorPos(out POINT p);
[DllImport("user32.dll")] public static extern int ShowCursor(bool bShow);
[DllImport("user32.dll")] public static extern bool GetCursorInfo(ref CURSORINFO pci);
[StructLayout(LayoutKind.Sequential)] public struct POINT { public int X; public int Y; }
[StructLayout(LayoutKind.Sequential)] public struct CURSORINFO {
    public int cbSize; public int flags; public IntPtr hCursor; public POINT ptScreenPos; }
'@

function Shot([string]$path) {
    $b = [System.Windows.Forms.SystemInformation]::VirtualScreen
    $bmp = New-Object System.Drawing.Bitmap $b.Width, $b.Height
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($b.Left, $b.Top, 0, 0, $bmp.Size)
    $g.Dispose()
    $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    Write-Host "鎴浘: $path"
}

function CursorVisible {
    $ci = New-Object Win.Native+CURSORINFO
    $ci.cbSize = [System.Runtime.InteropServices.Marshal]::SizeOf($ci)
    [void][Win.Native]::GetCursorInfo([ref]$ci)
    return (($ci.flags -band 1) -ne 0)
}

# ---- 0. 娓呭満 ---------------------------------------------------------------
Get-Process ministream-host, ministream-client -ErrorAction SilentlyContinue |
    ForEach-Object { $_.Kill(); $_.WaitForExit(3000) | Out-Null }
Start-Sleep -Milliseconds 500
Remove-Item (Join-Path $bin 'host.log') -ErrorAction SilentlyContinue
Remove-Item (Join-Path $bin 'client.log') -ErrorAction SilentlyContinue

# ---- 1. 璧疯鎺х -----------------------------------------------------------
$h = Start-Process -FilePath $hostExe -WorkingDirectory $bin -PassThru
Start-Sleep -Milliseconds 2000
if ($h.HasExited) { throw "琚帶绔惎鍔ㄥけ璐ワ紝閫€鍑虹爜 $($h.ExitCode)" }
Write-Host "琚帶绔?PID=$($h.Id)"

# ---- 2. 鎶婂厜鏍囨斁鍒板凡鐭ヤ綅缃?------------------------------------------------
[void][Win.Native]::SetCursorPos(600, 400)
Start-Sleep -Milliseconds 300
$vis0 = CursorVisible
Write-Host "琚帶绔厜鏍囧彲瑙佹€? $vis0"

# ---- 3. 璧疯繙鎺х锛堣嚜鍔ㄨ繛鎺?15 绉掑悗閫€鍑猴級----------------------------------
$cargs = @('--auto', '--host', '127.0.0.1', '--port', '14738', '--password', '123',
           '--seconds', '15', '--log-level', '1')
$c = Start-Process -FilePath $clientExe -WorkingDirectory $bin -ArgumentList $cargs -PassThru
Write-Host "杩滄帶绔?PID=$($c.Id)"

# ---- 4. 绛夐甯?+ 鍏夋爣褰㈢姸 --------------------------------------------------
Start-Sleep -Milliseconds 6000
[void][Win.Native]::SetCursorPos(600, 400)   # 淇濊瘉浣嶇疆鏄垜浠湡鏈涚殑
Start-Sleep -Milliseconds 600
Shot (Join-Path $outDir 'with_cursor.png')

# ---- 5. 妯℃嫙"娓告垙鍐呭満钘忓厜鏍?锛歋howCursor(FALSE) ---------------------------
$before = CursorVisible
for ($i = 0; $i -lt 20; $i++) { [void][Win.Native]::ShowCursor($false) }
Start-Sleep -Milliseconds 400
$after = CursorVisible
Write-Host "ShowCursor(FALSE) 涔嬪悗: 涔嬪墠鍙=$before 鐜板湪鍙=$after"
Start-Sleep -Milliseconds 1200
Shot (Join-Path $outDir 'cursor_hidden.png')

# 鎭㈠鍏夋爣
for ($i = 0; $i -lt 25; $i++) { [void][Win.Native]::ShowCursor($true) }

# ---- 6. 绛夎繙鎺х鑷繁閫€鍑猴紙--seconds 15锛?---------------------------------
$c.WaitForExit(25000) | Out-Null
Start-Sleep -Milliseconds 500
if (-not $h.HasExited) { $h.Kill() }
Start-Sleep -Milliseconds 500

# ---- 7. 姹囨€绘棩蹇?-----------------------------------------------------------
Write-Host "`n===== host.log 鍏夋爣鐩稿叧 ====="
Select-String -Path (Join-Path $bin 'host.log') -Pattern '鍏夋爣|Captur|閲囬泦宸插惎鍔▅鐩戝惉' |
    ForEach-Object { $_.Line } | Select-Object -Last 12
Write-Host "`n===== client.log 鍏夋爣鐩稿叧 ====="
Select-String -Path (Join-Path $bin 'client.log') -Pattern '鍏夋爣|涓叉祦|浼犺緭|鎺㈡祴|TCP' |
    ForEach-Object { $_.Line } | Select-Object -Last 25
Write-Host "`n===== 鎴浘 ====="
Get-ChildItem $outDir -Filter *.png | Select-Object Name, Length

