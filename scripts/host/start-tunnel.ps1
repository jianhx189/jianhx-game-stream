# -*- coding: utf-8 -*-
# =============================================================================
#  start-tunnel.ps1 —— 手动前台启动隧道（排障用）
#  用户在“启动隧道.bat”上双击时走这里；会一直占用当前窗口，Ctrl+C 退出。
# =============================================================================
$ErrorActionPreference = 'Stop'

$here = Split-Path -Parent $PSCommandPath
$root = Split-Path -Parent $here
. (Join-Path $here 'tunnel-common.ps1')

try {
    $settings = Get-MslinkHostSettings -Root $root
} catch {
    Write-Host ''
    Write-Host ('[错误] ' + $_.Exception.Message) -ForegroundColor Red
    exit 1
}

$exe = Join-Path $root 'mslink.exe'
if (-not (Test-Path -LiteralPath $exe)) {
    Write-Host ('[错误] 找不到 ' + $exe) -ForegroundColor Red
    exit 1
}

Write-Host '============================================================'
Write-Host (' MiniStream 被控端隧道  TCP ' + $settings.TcpPort + ' / UDP ' + $settings.UdpPort)
Write-Host (' Sunshine: ' + $settings.Target + '（同机）')
Write-Host ' 下面每 10 秒打印一次隧道统计；按 Ctrl+C 退出。'
Write-Host ' 如果开机自启任务已经在跑，这里会提示端口被占用。'
Write-Host '============================================================'
Write-Host ''

if (Test-TcpPortListening -Port $settings.TcpPort) {
    Write-Host ('[提示] TCP ' + $settings.TcpPort + ' 已经在监听，说明隧道很可能已经在运行。') -ForegroundColor Yellow
    Write-Host '        先运行 卸载.bat 或结束 mslink.exe，再手动启动，避免端口冲突。'
    exit 0
}

exit (Start-MslinkTunnel -Settings $settings -Wait)
