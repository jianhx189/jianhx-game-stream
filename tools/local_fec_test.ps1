# 本地回环验证：模拟 UDP 丢包，检查 FEC 恢复与「缺帧等 IDR」是否工作
# 用法：powershell -File tools\local_fec_test.ps1 [-LossPct 5] [-Seconds 20]
param(
  [int]$LossPct = 5,
  [int]$Seconds = 18
)
$ErrorActionPreference = 'Stop'
$bin = 'D:\远程项目\build\msvc\bin'
Get-Process ministream* -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 600

$hostLog = Join-Path $bin 'host.log'
$cliLog  = Join-Path $bin 'client.log'
foreach ($f in @($hostLog, $cliLog)) { if (Test-Path $f) { Remove-Item $f -Force } }

$env:MINISTREAM_TEST_DISABLE_INPUT = '1'
$env:MINISTREAM_TEST_LOSS_PCT = "$LossPct"
$hostP = Start-Process -FilePath (Join-Path $bin 'ministream-host.exe') `
  -ArgumentList '--port','14738','--password','123','--log-level','1' `
  -WorkingDirectory $bin -PassThru -WindowStyle Hidden
Start-Sleep -Seconds 2

$env:MINISTREAM_NO_INPUT = '1'
$cliP = Start-Process -FilePath (Join-Path $bin 'ministream-client.exe') `
  -ArgumentList '--auto','--host','127.0.0.1','--port','14738','--password','123',
                '--fullscreen','0','--log-level','1','--seconds',"$($Seconds + 6)" `
  -WorkingDirectory $bin -PassThru

Start-Sleep -Seconds ($Seconds + 2)
Get-Process ministream-client -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 500
Get-Process ministream-host -ErrorAction SilentlyContinue | Stop-Process -Force

Write-Host ''
Write-Host "==== 客户端串流统计（模拟丢包 $LossPct%）===="
if (Test-Path $cliLog) {
  Select-String -Path $cliLog -Pattern '\[串流\]|FEC|帧号不连续|等 IDR|等IDR|切到 TCP|切回 UDP|自适应码率|自适应 FEC' |
    Select-Object -Last 40 | ForEach-Object { $_.Line }
} else { Write-Host 'client.log 不存在' }

Write-Host ''
Write-Host '==== 被控端关键行 ===='
if (Test-Path $hostLog) {
  Select-String -Path $hostLog -Pattern '模拟 UDP 丢包|视频 FEC|自适应 FEC|关键帧间隔|intra' |
    Select-Object -First 15 | ForEach-Object { $_.Line }
} else { Write-Host 'host.log 不存在' }
