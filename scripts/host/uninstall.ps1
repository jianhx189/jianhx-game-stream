# -*- coding: utf-8 -*-
# =============================================================================
#  uninstall.ps1 —— MiniStream 被控端卸载（由 卸载.bat 以管理员身份调用）
#
#  删除：两条计划任务、两条防火墙规则、安装目录（含 Sunshine 配置/配对记录）。
#  参数：
#    -TargetDir          安装目录，默认本脚本所在目录的上一级
#    -TaskName           隧道任务名，默认 MiniStreamTunnel
#    -SunshineTaskName   Sunshine 任务名，默认 MiniStreamSunshine
#    -FirewallPrefix     防火墙规则前缀，默认 "MiniStream Tunnel"
#    -Unattended         不询问确认（自检脚本用）
#    -LogPath            卸载日志路径，默认 %TEMP%\ministream-uninstall.log
# =============================================================================
param(
    [string]$TargetDir = '',
    [string]$TaskName = 'MiniStreamTunnel',
    [string]$SunshineTaskName = 'MiniStreamSunshine',
    [string]$FirewallPrefix = 'MiniStream Tunnel',
    [switch]$Unattended,
    [switch]$NoPause,
    [string]$LogPath = ''
)

$ErrorActionPreference = 'Stop'

$InternalDir = $PSScriptRoot
$DefaultRoot = Split-Path -Parent $InternalDir

if (-not $TargetDir) { $TargetDir = $DefaultRoot }
$TargetDir = [System.IO.Path]::GetFullPath($TargetDir).TrimEnd('\')

if (-not $LogPath) {
    $LogPath = Join-Path $env:TEMP 'ministream-uninstall.log'
}
$logParent = Split-Path -Parent ([System.IO.Path]::GetFullPath($LogPath))
if ($logParent -and -not (Test-Path -LiteralPath $logParent)) {
    New-Item -ItemType Directory -Force -Path $logParent | Out-Null
}

. (Join-Path $InternalDir 'tunnel-common.ps1')

function Log {
    param([string]$message)
    $line = '[' + (Get-Date -Format 'HH:mm:ss') + '] ' + $message
    Write-Host $line
    Add-LogLine -Path $LogPath -Text $message
}

function Assert-Administrator {
    $identity = [System.Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object System.Security.Principal.WindowsPrincipal($identity)
    if (-not $principal.IsInRole([System.Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw '需要管理员权限：请双击 卸载.bat，并在“用户账户控制”里点“是”。'
    }
}

function Assert-SafeTarget {
    param([string]$Path)
    $full = [System.IO.Path]::GetFullPath($Path).TrimEnd('\')
    if ($full -eq [System.IO.Path]::GetPathRoot($full).TrimEnd('\')) {
        throw "拒绝卸载盘符根目录：$full"
    }
    foreach ($f in @($env:windir, [Environment]::GetFolderPath('ProgramFiles'), [Environment]::GetFolderPath('Windows'))) {
        if ($f -and $full -eq ([System.IO.Path]::GetFullPath($f).TrimEnd('\'))) {
            throw "拒绝卸载系统目录：$full"
        }
    }
    if ((Split-Path -Leaf $full) -ne 'MiniStream') {
        throw "拒绝卸载：目标目录名不是 MiniStream（$full）。如确需卸载，请显式指定 -TargetDir。"
    }
}

function Remove-TaskIfExists {
    param([string]$Name)
    if (-not $Name) { return }
    [void](Invoke-QuietCommand ('schtasks /end /tn "' + $Name + '"'))
    [void](Invoke-QuietCommand ('schtasks /delete /tn "' + $Name + '" /f'))
    $stillThere = $true
    try {
        $null = Get-ScheduledTask -TaskName $Name -ErrorAction Stop
    } catch {
        $stillThere = $false
    }
    if ($stillThere) {
        throw "计划任务仍然存在：$Name"
    }
    Log "计划任务已删除：$Name"
}

function Stop-TargetProcesses {
    param([string]$Root)
    $prefix = $Root.TrimEnd('\') + '\'
    $procs = Get-CimInstance Win32_Process -ErrorAction SilentlyContinue | Where-Object {
        $_.ExecutablePath -and $_.ExecutablePath.StartsWith($prefix, [System.StringComparison]::OrdinalIgnoreCase)
    }
    foreach ($p in $procs) {
        try {
            Stop-Process -Id $p.ProcessId -Force -ErrorAction SilentlyContinue
            Log "已结束进程：$($p.ExecutablePath)（PID $($p.ProcessId)）"
        } catch {
        }
    }
    $launchers = Get-CimInstance Win32_Process -Filter "Name='powershell.exe'" -ErrorAction SilentlyContinue | Where-Object {
        $_.CommandLine -and $_.CommandLine.IndexOf($prefix, [System.StringComparison]::OrdinalIgnoreCase) -ge 0 -and
        $_.CommandLine.IndexOf('run-tunnel.ps1', [System.StringComparison]::OrdinalIgnoreCase) -ge 0
    }
    foreach ($p in $launchers) {
        try {
            Stop-Process -Id $p.ProcessId -Force -ErrorAction SilentlyContinue
        } catch {
        }
    }
    Start-Sleep -Milliseconds 500
}

try {
    Log '============================================================'
    Log ' MiniStream 被控端 卸载'
    Log '============================================================'

    Assert-Administrator
    Assert-SafeTarget -Path $TargetDir

    if (-not $Unattended) {
        Write-Host ''
        Write-Host ('即将删除：' + $TargetDir)
        Write-Host '（包含 Sunshine 的配置、账号和配对记录，删除后不可恢复）'
        $answer = Read-Host '确认卸载请输入 Y，其它任意键取消'
        if ($answer -notmatch '^[Yy]') {
            Log '用户取消卸载'
            Log 'UNINSTALL_RESULT=CANCELLED'
            exit 0
        }
    }

    Log "安装目录：$TargetDir"

    Log '[1/4] 停止并删除计划任务…'
    Remove-TaskIfExists -Name $TaskName
    Remove-TaskIfExists -Name $SunshineTaskName

    Log '[2/4] 结束相关进程…'
    Stop-TargetProcesses -Root $TargetDir

    Log '[3/4] 删除防火墙规则…'
    foreach ($proto in @('TCP', 'UDP')) {
        $name = "$FirewallPrefix $proto"
        [void](Invoke-QuietCommand ('netsh advfirewall firewall delete rule name="' + $name + '"'))
        Log "防火墙规则已删除（如存在）：$name"
    }

    Log '[4/4] 删除安装目录…'
    $selfInsideTarget = $InternalDir.StartsWith($TargetDir + '\', [System.StringComparison]::OrdinalIgnoreCase)
    if (Test-Path -LiteralPath $TargetDir) {
        if ($selfInsideTarget) {
            # 脚本自己在待删除目录里：交给独立的清理进程。
            # 两个坑：
            #   1) 用户可能还停在“按任意键继续”，或外层/内层 cmd 还没退出，
            #      它们会占用目录里的 .bat 文件；
            #   2) 进程的当前目录如果还在待删目录里，删除会失败。
            # 所以清理进程先等“命令行里含安装目录的 cmd/powershell”全部退出，
            # 再循环删除，避免把还在执行的 卸载.bat 删掉导致 cmd 报错。
            $cleanupPs1 = Join-Path $env:TEMP 'ministream-uninstall-cleanup.ps1'
            $cleanupTemplate = @'
$ErrorActionPreference = 'SilentlyContinue'
$target = '__TARGET__'
$deadline = (Get-Date).AddMinutes(10)
while ((Get-Date) -lt $deadline) {
    $busy = @(Get-CimInstance Win32_Process | Where-Object {
        $_.ProcessId -ne $PID -and $_.CommandLine -and
        ($_.Name -eq 'cmd.exe' -or $_.Name -eq 'powershell.exe') -and
        $_.CommandLine.IndexOf($target, [System.StringComparison]::OrdinalIgnoreCase) -ge 0
    })
    if ($busy.Count -eq 0) { break }
    Start-Sleep -Seconds 1
}
for ($i = 0; $i -lt 120; $i++) {
    if (-not (Test-Path -LiteralPath $target)) { exit 0 }
    try {
        Remove-Item -LiteralPath $target -Recurse -Force -ErrorAction Stop
        exit 0
    } catch {
        Start-Sleep -Seconds 1
    }
}
exit 1
'@
            $cleanup = $cleanupTemplate.Replace('__TARGET__', $TargetDir.Replace("'", "''"))
            [System.IO.File]::WriteAllText($cleanupPs1, ($cleanup -replace "`r?`n", "`r`n"), (New-Object System.Text.UTF8Encoding($true)))
            $psExe = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
            Start-Process -FilePath $psExe `
                -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $cleanupPs1) `
                -WorkingDirectory $env:TEMP -WindowStyle Hidden | Out-Null
            Log '  已安排后台清理进程删除安装目录（关闭本窗口后自动完成）'
        } else {
            Remove-Item -LiteralPath $TargetDir -Recurse -Force
            Log '  安装目录已删除'
        }
    } else {
        Log '  安装目录不存在，跳过'
    }

    Log ''
    Log '卸载完成。'
    Log 'UNINSTALL_RESULT=OK'
    exit 0
} catch {
    Log ''
    Log ('[失败] ' + $_.Exception.Message)
    Log ('UNINSTALL_RESULT=FAIL: ' + $_.Exception.Message)
    exit 1
}
