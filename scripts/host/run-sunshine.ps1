# -*- coding: utf-8 -*-
# =============================================================================
#  run-sunshine.ps1 —— 由计划任务在登录时调用，启动被控端 Sunshine
#
#  必须做单实例检查：
#    Sunshine 便携版在某些情况下会启动出两个 sunshine.exe 进程，
#    Windows 的 SO_REUSEADDR 允许它们同时 bind 0.0.0.0:47984/47989，
#    表现为 “serverinfo 时通时不通 / 配对随机失败 / 连接被拒”。
#    所以这里先看 47989 是否已被监听、是否已有 sunshine.exe；
#    已有就直接复用，绝不再起第二个。
#
#  诊断日志：<安装目录>\run-sunshine.log
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

$sunshineDir = Join-Path $TargetDir 'sunshine'
$sunshineExe = Join-Path $sunshineDir 'sunshine.exe'
$logPath = Join-Path $TargetDir 'run-sunshine.log'

function Write-SunLog {
    param([AllowEmptyString()][string]$message)
    Add-LogLine -Path $logPath -Text $message
}

$mutex = $null
try {
    # 防止两个启动器同时通过“单实例检查”后各起一个
    try {
        $mutex = New-Object System.Threading.Mutex($false, 'Global\MiniStreamSunshineLauncher')
        if (-not $mutex.WaitOne(5000)) {
            Write-SunLog '另一个 Sunshine 启动器正在运行，跳过本次启动'
            exit 0
        }
    } catch {
        $mutex = $null
    }

    if (-not (Test-Path -LiteralPath $sunshineExe)) {
        Write-SunLog ("找不到 Sunshine：$sunshineExe")
        exit 1
    }

    $existing = @(Get-Process -Name 'sunshine' -ErrorAction SilentlyContinue)
    $portListening = Test-TcpPortListening -Port 47989

    if ($existing.Count -gt 0 -or $portListening) {
        $pids = @($existing | ForEach-Object { $_.Id }) -join ','
        if (-not $pids) { $pids = '未知' }
        Write-SunLog ("检测到已有 Sunshine 实例（pid=$pids，47989 监听=$portListening），复用，不再启动第二个")
        exit 0
    }

    # -WindowStyle Hidden：sunshine.exe 是控制台程序，不加这个会留一个黑窗口。
    # Sunshine 自己会常驻系统托盘图标，隐藏控制台不影响使用。
    $proc = Start-Process -FilePath $sunshineExe -WorkingDirectory $sunshineDir -WindowStyle Hidden -PassThru
    Write-SunLog ("已启动 Sunshine（pid=" + $proc.Id + "，控制台窗口已隐藏）")

    # 等 47989 起来，顺便复查有没有多出来实例
    $deadline = (Get-Date).AddSeconds(20)
    while ((Get-Date) -lt $deadline) {
        if (Test-TcpPortListening -Port 47989) { break }
        Start-Sleep -Milliseconds 500
    }

    if (Test-TcpPortListening -Port 47989) {
        Write-SunLog 'Sunshine 已在 47989 监听'
    } else {
        Write-SunLog '警告：20 秒内没有检测到 47989 监听，Sunshine 可能启动失败'
    }

    $after = @(Get-Process -Name 'sunshine' -ErrorAction SilentlyContinue)
    if ($after.Count -gt 1) {
        $pids = @($after | ForEach-Object { $_.Id }) -join ','
        Write-SunLog ("警告：检测到 $($after.Count) 个 sunshine.exe 进程（pid=$pids）。" +
            'Sunshine 便携版可能重复启动；若出现 serverinfo 时通时不通/配对失败，请结束多余实例。')
    }
} catch {
    Write-SunLog ('启动 Sunshine 失败：' + $_.Exception.Message)
    exit 1
} finally {
    if ($mutex) {
        try { $mutex.ReleaseMutex() } catch { }
        try { $mutex.Dispose() } catch { }
    }
}
