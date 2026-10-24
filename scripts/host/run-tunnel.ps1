# -*- coding: utf-8 -*-
# =============================================================================
#  run-tunnel.ps1 —— 由计划任务在登录时调用：把 mslink host 以隐藏窗口拉起
#
#  为什么通过 PowerShell 再启动：
#    mslink.exe 是控制台程序，计划任务直接启动会留一个黑窗口。这里用
#    Start-Process -WindowStyle Hidden 起子进程，登录后用户看不到窗口。
#
#  手动调试请看 启动隧道.bat（那个是前台运行，会打印每 10 秒的统计）。
# =============================================================================
param(
    [string]$TargetDir = ''
)

$ErrorActionPreference = 'Stop'

$here = Split-Path -Parent $PSCommandPath
. (Join-Path $here 'tunnel-common.ps1')

if (-not $TargetDir) {
    $TargetDir = Split-Path -Parent $here
}
$TargetDir = [System.IO.Path]::GetFullPath($TargetDir).TrimEnd('\')

try {
    $settings = Get-MslinkHostSettings -Root $TargetDir
    $exe = Join-Path $TargetDir 'mslink.exe'
    if (-not (Test-Path -LiteralPath $exe)) {
        throw "找不到 $exe"
    }

    $proc = Start-MslinkTunnel -Settings $settings

    # 留一个 PID 文件，排障时能直接看出“隧道到底起没起”
    try {
        Set-Content -LiteralPath (Join-Path $TargetDir 'mslink.pid') -Value $proc.Id -Encoding Ascii
    } catch {
    }
} catch {
    # 计划任务里看不到窗口，把失败原因写进日志
    $log = Join-Path $TargetDir 'run-tunnel-error.log'
    Add-LogLine -Path $log -Text ("启动隧道失败：" + $_.Exception.Message)
    exit 1
}
