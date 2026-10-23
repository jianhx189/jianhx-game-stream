# 本地端到端音频验证：被控端播放 440Hz 正弦 -> 远控端日志里应出现 RMS>0 的音频包
$ErrorActionPreference = 'Stop'
$bin = 'D:\远程项目\build\msvc\bin'
$logDir = 'D:\远程项目\build\client-side'
New-Item -ItemType Directory -Force -Path $logDir | Out-Null

# 停掉所有 ministream 进程（含交付版被控端；测完由部署步骤重新拉起）
Get-Process ministream* -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 600

# 生成 6 秒 440Hz 立体声 48k WAV
$wav = Join-Path $env:TEMP 'ms_tone.wav'
$sr = 48000; $sec = 6; $freq = 440.0
$n = $sr * $sec
$bytes = New-Object byte[] ($n * 4)
for ($i = 0; $i -lt $n; $i++) {
  $v = [int16](10000 * [math]::Sin(2 * [math]::PI * $freq * $i / $sr))
  $b = [BitConverter]::GetBytes($v)
  $bytes[$i*4] = $b[0]; $bytes[$i*4+1] = $b[1]
  $bytes[$i*4+2] = $b[0]; $bytes[$i*4+3] = $b[1]
}
$fs = [System.IO.File]::Create($wav)
$bw = New-Object System.IO.BinaryWriter($fs)
$bw.Write([Text.Encoding]::ASCII.GetBytes('RIFF')); $bw.Write([int32](36 + $n*4))
$bw.Write([Text.Encoding]::ASCII.GetBytes('WAVE'))
$bw.Write([Text.Encoding]::ASCII.GetBytes('fmt ')); $bw.Write([int32]16)
$bw.Write([int16]1); $bw.Write([int16]2); $bw.Write([int32]$sr)
$bw.Write([int32]($sr*4)); $bw.Write([int16]4); $bw.Write([int16]16)
$bw.Write([Text.Encoding]::ASCII.GetBytes('data')); $bw.Write([int32]($n*4))
$bw.Write($bytes); $bw.Close(); $fs.Close()

$hostLog = Join-Path $bin 'host.log'
$cliLog = Join-Path $bin 'client.log'
foreach ($f in @($hostLog, $cliLog)) { if (Test-Path $f) { Remove-Item $f -Force } }

$env:MINISTREAM_TEST_DISABLE_INPUT = '1'
$hostP = Start-Process -FilePath (Join-Path $bin 'ministream-host.exe') `
  -ArgumentList '--port','14738','--password','123','--log-level','1' `
  -WorkingDirectory $bin -PassThru -WindowStyle Hidden
Start-Sleep -Seconds 2

$env:MINISTREAM_NO_INPUT = '1'
$env:MINISTREAM_MOUSE_MODE = 'absolute'
$env:MINISTREAM_AUDIO = '1'
$cliP = Start-Process -FilePath (Join-Path $bin 'ministream-client.exe') `
  -ArgumentList '--auto','--host','127.0.0.1','--port','14738','--password','123',
                '--fullscreen','0','--log-level','1','--seconds','30' `
  -WorkingDirectory $bin -PassThru

$t0 = Get-Date
while (((Get-Date) - $t0).TotalSeconds -lt 30) {
  if ((Test-Path $cliLog) -and (Select-String -Path $cliLog -Pattern '进入主循环' -Quiet)) { break }
  Start-Sleep -Milliseconds 300
}
Start-Sleep -Seconds 3
Write-Host '[tone] 播放 440Hz（异步 6 秒，循环播放）...'
$player = New-Object System.Media.SoundPlayer $wav
$player.PlayLooping()
Start-Sleep -Seconds 8
$player.Stop()

Write-Host ''
Write-Host '===== 被控端音频日志 ====='
Select-String -Path $hostLog -Pattern '音频' | ForEach-Object { $_.Line }
Write-Host ''
Write-Host '===== 远控端音频/声音日志 ====='
Select-String -Path $cliLog -Pattern '音频|声音|崩溃|错误' | ForEach-Object { $_.Line }

Get-Process ministream* -ErrorAction SilentlyContinue | Stop-Process -Force
