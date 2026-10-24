# -*- coding: utf-8 -*-
# =============================================================================
#  install.ps1 —— MiniStream 被控端安装脚本（由 安装.bat 以管理员身份调用）
#
#  做四件事：
#    1. 把便携版 Sunshine + mslink.exe + 配置复制到安装目录
#    2. 设置 Sunshine 网页管理账号（免去首次打开网页手工建账号）
#    3. 注册两条登录自启计划任务（Sunshine / 隧道）+ 放行 1 TCP + 1 UDP 防火墙
#    4. 立即启动，并做一次基本自检
#
#  参数（正常用户双击 安装.bat 不需要任何参数；自检脚本会用参数装到测试目录）：
#    -TargetDir            安装目录，默认 %ProgramFiles%\MiniStream
#    -TaskName             隧道计划任务名，默认 MiniStreamTunnel
#    -SunshineTaskName     Sunshine 计划任务名，默认 MiniStreamSunshine
#    -FirewallPrefix       防火墙规则名前缀，默认 "MiniStream Tunnel"
#    -WebUser              网页管理用户名，默认 admin
#    -WebPassword          网页管理口令，留空则随机生成
#    -ResetWebPassword     强制重设网页口令
#    -SkipSunshineCreds    不设置网页账号（测试/调试用）
#    -SkipSunshineLaunch   不启动 Sunshine（测试用，避免和已在运行的实例抢端口）
#    -SkipTunnelStart      不启动隧道（测试用）
#    -ForceConfig          用包里的 mslink-host.ini 覆盖安装目录里的配置
#    -LogPath              安装日志路径
# =============================================================================
param(
    [string]$TargetDir = '',
    [string]$TaskName = 'MiniStreamTunnel',
    [string]$SunshineTaskName = 'MiniStreamSunshine',
    [string]$FirewallPrefix = 'MiniStream Tunnel',
    [string]$WebUser = 'admin',
    [string]$WebPassword = '',
    [switch]$ResetWebPassword,
    [switch]$SkipSunshineCreds,
    [switch]$SkipSunshineLaunch,
    [switch]$SkipTunnelStart,
    [switch]$ForceConfig,
    [switch]$NoPause,
    [string]$LogPath = ''
)

$ErrorActionPreference = 'Stop'

$InternalDir = $PSScriptRoot
$SourceDir = Split-Path -Parent $InternalDir

if (-not $TargetDir) {
    $TargetDir = Join-Path ([Environment]::GetFolderPath('ProgramFiles')) 'MiniStream'
}
$TargetDir = [System.IO.Path]::GetFullPath($TargetDir).TrimEnd('\')

if (-not $LogPath) {
    $logDir = Join-Path $env:ProgramData 'MiniStream'
    New-Item -ItemType Directory -Force -Path $logDir | Out-Null
    $LogPath = Join-Path $logDir 'install.log'
}
$logParent = Split-Path -Parent ([System.IO.Path]::GetFullPath($LogPath))
if ($logParent -and -not (Test-Path -LiteralPath $logParent)) {
    New-Item -ItemType Directory -Force -Path $logParent | Out-Null
}

. (Join-Path $InternalDir 'tunnel-common.ps1')

function Log {
    param([AllowEmptyString()][string]$message)
    $line = '[' + (Get-Date -Format 'HH:mm:ss') + '] ' + $message
    Write-Host $line
    Add-LogLine -Path $LogPath -Text $message
}

function Assert-Administrator {
    $identity = [System.Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object System.Security.Principal.WindowsPrincipal($identity)
    if (-not $principal.IsInRole([System.Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw '需要管理员权限：请双击 安装.bat，并在“用户账户控制”里点“是”。'
    }
}

function Assert-SafeTarget {
    param([string]$Path)
    $full = [System.IO.Path]::GetFullPath($Path).TrimEnd('\')
    if ($full -eq [System.IO.Path]::GetPathRoot($full).TrimEnd('\')) {
        throw "安装目录不能是盘符根目录：$full"
    }
    $forbidden = @(
        $env:windir,
        $env:SystemDrive,
        [Environment]::GetFolderPath('ProgramFiles'),
        [Environment]::GetFolderPath('Windows')
    )
    foreach ($f in $forbidden) {
        if ($f -and $full -eq ([System.IO.Path]::GetFullPath($f).TrimEnd('\'))) {
            throw "拒绝安装到系统目录：$full"
        }
    }
    if ($full -eq [System.IO.Path]::GetFullPath($SourceDir).TrimEnd('\')) {
        throw "安装目录不能等于压缩包目录：$full"
    }
}

function Invoke-Robocopy {
    param(
        [Parameter(Mandatory = $true)][string]$From,
        [Parameter(Mandatory = $true)][string]$To,
        [string[]]$ExcludeDirs = @(),
        [string[]]$ExcludeFiles = @()
    )
    $arguments = @($From, $To, '/E', '/R:1', '/W:1', '/NFL', '/NDL', '/NJH', '/NJS', '/NP')
    if ($ExcludeDirs.Count -gt 0) {
        $arguments += '/XD'
        $arguments += $ExcludeDirs
    }
    if ($ExcludeFiles.Count -gt 0) {
        $arguments += '/XF'
        $arguments += $ExcludeFiles
    }
    & robocopy.exe @arguments | Out-Null
    $code = $LASTEXITCODE
    if ($code -ge 8) {
        throw "复制文件失败（robocopy 退出码 $code）：$From -> $To"
    }
}

function Stop-TargetProcesses {
    param([string]$Root)
    $prefix = $Root.TrimEnd('\') + '\'

    # 先停计划任务，避免任务把进程又拉起来
    foreach ($name in @($TaskName, $SunshineTaskName)) {
        if ($name) {
            [void](Invoke-QuietCommand ('schtasks /end /tn "' + $name + '"'))
        }
    }

    # 再按可执行文件路径杀本安装目录下的进程（绝不碰 D:\build\sunshine 里正在跑的实例）
    $procs = Get-CimInstance Win32_Process -ErrorAction SilentlyContinue | Where-Object {
        $_.ExecutablePath -and $_.ExecutablePath.StartsWith($prefix, [System.StringComparison]::OrdinalIgnoreCase)
    }
    foreach ($p in $procs) {
        try {
            Stop-Process -Id $p.ProcessId -Force -ErrorAction SilentlyContinue
        } catch {
        }
    }

    # run-tunnel.ps1 启动器（它的命令行里含安装目录）
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

function Set-FirewallRules {
    param([int]$Tcp, [int]$Udp, [string]$Prefix)

    $rules = @(
        @{ Proto = 'TCP'; Port = $Tcp },
        @{ Proto = 'UDP'; Port = $Udp }
    )
    foreach ($item in $rules) {
        $proto = $item.Proto
        $port = $item.Port
        $name = "$Prefix $proto"
        [void](Invoke-QuietCommand ('netsh advfirewall firewall delete rule name="' + $name + '"'))
        $code = Invoke-QuietCommand ('netsh advfirewall firewall add rule name="' + $name + '" dir=in action=allow protocol=' + $proto + ' localport=' + $port + ' profile=any enable=yes')
        if ($code -ne 0) {
            throw "创建防火墙规则失败：$name（端口 $port/$proto），netsh 退出码 $code"
        }
        # 用 PowerShell 复核，避免解析 netsh 的本地化输出
        $rule = Get-NetFirewallRule -DisplayName $name -ErrorAction SilentlyContinue | Select-Object -First 1
        if (-not $rule) {
            throw "防火墙规则校验失败（查不到）：$name"
        }
        $pf = $rule | Get-NetFirewallPortFilter
        $ports = @($pf.LocalPort | ForEach-Object { [string]$_ })
        if (($rule.Enabled -ne $true) -or ($rule.Direction -ne 'Inbound') -or ($rule.Action -ne 'Allow') -or
            ($ports -notcontains [string]$port) -or ([string]$pf.Protocol -ne $proto)) {
            throw "防火墙规则校验失败：$name（protocol=$($pf.Protocol) port=$($ports -join ',') dir=$($rule.Direction) action=$($rule.Action)）"
        }
        Log "防火墙已放行 $proto $port（规则名：$name）"
    }
}

function ConvertTo-XmlText {
    param([AllowEmptyString()][string]$Text)
    if (-not $Text) { return '' }
    return [System.Security.SecurityElement]::Escape($Text)
}

function New-TaskXml {
    param(
        [string]$Name,
        [string]$Command,
        [string]$Arguments,
        [string]$WorkingDirectory,
        [string]$Description,
        [string]$UserId,
        [bool]$Hidden,
        [string]$Delay
    )

    $cmd = ConvertTo-XmlText $Command
    $arg = ConvertTo-XmlText $Arguments
    $wd = ConvertTo-XmlText $WorkingDirectory
    $desc = ConvertTo-XmlText $Description
    $uid = ConvertTo-XmlText $UserId
    $hiddenText = if ($Hidden) { 'true' } else { 'false' }

    return @"
<?xml version="1.0" encoding="UTF-16"?>
<Task version="1.4" xmlns="http://schemas.microsoft.com/windows/2004/02/mit/task">
  <RegistrationInfo>
    <Description>$desc</Description>
    <URI>\$Name</URI>
  </RegistrationInfo>
  <Triggers>
    <LogonTrigger>
      <Enabled>true</Enabled>
      <UserId>$uid</UserId>
      <Delay>$Delay</Delay>
    </LogonTrigger>
  </Triggers>
  <Principals>
    <Principal id="Author">
      <UserId>$uid</UserId>
      <LogonType>InteractiveToken</LogonType>
      <RunLevel>HighestAvailable</RunLevel>
    </Principal>
  </Principals>
  <Settings>
    <MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>
    <DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>
    <StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>
    <AllowHardTerminate>true</AllowHardTerminate>
    <StartWhenAvailable>true</StartWhenAvailable>
    <RunOnlyIfNetworkAvailable>false</RunOnlyIfNetworkAvailable>
    <IdleSettings>
      <StopOnIdleEnd>false</StopOnIdleEnd>
      <RestartOnIdle>false</RestartOnIdle>
    </IdleSettings>
    <AllowStartOnDemand>true</AllowStartOnDemand>
    <Enabled>true</Enabled>
    <Hidden>$hiddenText</Hidden>
    <RunOnlyIfIdle>false</RunOnlyIfIdle>
    <WakeToRun>false</WakeToRun>
    <ExecutionTimeLimit>PT0S</ExecutionTimeLimit>
    <Priority>4</Priority>
    <RestartOnFailure>
      <Interval>PT1M</Interval>
      <Count>3</Count>
    </RestartOnFailure>
  </Settings>
  <Actions Context="Author">
    <Exec>
      <Command>$cmd</Command>
      <Arguments>$arg</Arguments>
      <WorkingDirectory>$wd</WorkingDirectory>
    </Exec>
  </Actions>
</Task>
"@
}

function Register-TaskFromXml {
    param([string]$Name, [string]$XmlText)

    [void](Invoke-QuietCommand ('schtasks /delete /tn "' + $Name + '" /f'))

    $tmp = Join-Path $env:TEMP ('ministream-task-' + [guid]::NewGuid().ToString('N') + '.xml')
    $outFile = Join-Path $env:TEMP ('ministream-task-' + [guid]::NewGuid().ToString('N') + '.out')
    $errFile = $outFile + '.err'
    try {
        [System.IO.File]::WriteAllText($tmp, $XmlText, [System.Text.Encoding]::Unicode)
        $p = Start-Process -FilePath 'schtasks.exe' -ArgumentList @('/create', '/tn', $Name, '/xml', $tmp, '/f') `
            -Wait -PassThru -WindowStyle Hidden -RedirectStandardOutput $outFile -RedirectStandardError $errFile
        if ($p.ExitCode -ne 0) {
            $detail = ''
            if (Test-Path -LiteralPath $outFile) { $detail += (Get-Content -LiteralPath $outFile -Raw -ErrorAction SilentlyContinue) }
            if (Test-Path -LiteralPath $errFile) { $detail += (Get-Content -LiteralPath $errFile -Raw -ErrorAction SilentlyContinue) }
            throw "注册计划任务失败（$Name，退出码 $($p.ExitCode)）：$detail"
        }
    } finally {
        Remove-Item -LiteralPath $tmp -Force -ErrorAction SilentlyContinue
        Remove-Item -LiteralPath $outFile -Force -ErrorAction SilentlyContinue
        Remove-Item -LiteralPath $errFile -Force -ErrorAction SilentlyContinue
    }

    try {
        $null = Get-ScheduledTask -TaskName $Name -ErrorAction Stop
    } catch {
        throw "注册后查不到计划任务：$Name"
    }
    Log "计划任务已注册：$Name"
}

function Wait-TunnelReady {
    param([int]$Tcp, [int]$Udp, [int]$TimeoutSeconds = 15)

    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    while ((Get-Date) -lt $deadline) {
        $tcpOk = $false
        $udpOk = $false
        try {
            $tcpOk = $null -ne (Get-NetTCPConnection -LocalPort $Tcp -State Listen -ErrorAction Stop)
        } catch {
        }
        try {
            $udpOk = $null -ne (Get-NetUDPEndpoint -LocalPort $Udp -ErrorAction Stop)
        } catch {
        }
        if ($tcpOk -and $udpOk) { return $true }
        Start-Sleep -Milliseconds 500
    }
    return $false
}

# ---------------------------------------------------------------------------
#  主流程
# ---------------------------------------------------------------------------
try {
    Log '============================================================'
    Log ' MiniStream 被控端 安装'
    Log '============================================================'

    Assert-Administrator
    Assert-SafeTarget -Path $TargetDir

    Log "安装目录：$TargetDir"

    if (-not (Test-Path -LiteralPath (Join-Path $SourceDir 'mslink.exe'))) {
        throw "安装包不完整：$SourceDir 里找不到 mslink.exe"
    }

    # ---- 1. 停旧进程，复制文件 -------------------------------------------
    Log '[1/6] 停止旧进程、复制文件…'
    Stop-TargetProcesses -Root $TargetDir
    New-Item -ItemType Directory -Force -Path $TargetDir | Out-Null

    if ([System.IO.Path]::GetFullPath($SourceDir).TrimEnd('\') -ne $TargetDir) {
        # Sunshine 便携版完整目录（不覆盖安装目录里已有的 config，避免丢配对/账号）
        Invoke-Robocopy -From (Join-Path $SourceDir 'sunshine') -To (Join-Path $TargetDir 'sunshine') `
            -ExcludeDirs @('config') -ExcludeFiles @('*.log')
        # 根目录文件（不改 mslink-host.ini，单独处理）
        Get-ChildItem -LiteralPath $SourceDir -File | Where-Object {
            $_.Name -ne 'mslink-host.ini'
        } | ForEach-Object {
            Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $TargetDir $_.Name) -Force
        }
        Invoke-Robocopy -From (Join-Path $SourceDir '_internal') -To (Join-Path $TargetDir '_internal')
    } else {
        Log '  已在安装目录内运行，跳过文件复制'
    }

    if (Test-Path -LiteralPath (Join-Path $SourceDir 'console')) { Invoke-Robocopy -From (Join-Path $SourceDir 'console') -To (Join-Path $TargetDir 'console') }
    $sunshineDir = Join-Path $TargetDir 'sunshine'
    $sunshineExe = Join-Path $sunshineDir 'sunshine.exe'
    $mslinkExe = Join-Path $TargetDir 'mslink.exe'
    foreach ($f in @($sunshineExe, (Join-Path $sunshineDir 'assets\apps.json'), $mslinkExe)) {
        if (-not (Test-Path -LiteralPath $f)) { throw "安装后缺少文件：$f" }
    }

    # ---- 2. 配置 ---------------------------------------------------------
    Log '[2/6] 处理配置…'
    $targetIni = Join-Path $TargetDir 'mslink-host.ini'
    $packageIni = Join-Path $SourceDir 'mslink-host.ini'
    if ($ForceConfig -or -not (Test-Path -LiteralPath $targetIni)) {
        Copy-Item -LiteralPath $packageIni -Destination $targetIni -Force
        Log '  已写入 mslink-host.ini（安装包默认配置）'
    } else {
        Log '  沿用安装目录里已有的 mslink-host.ini（保留用户改动）'
    }
    $settings = Get-MslinkHostSettings -Root $TargetDir
    $installUserId = [System.Security.Principal.WindowsIdentity]::GetCurrent().Name
    foreach ($privateFile in @($targetIni, $LogPath)) {
        Protect-MiniStreamFile -Path $privateFile -UserId $installUserId
    }
    Log ("  隧道端口：TCP " + $settings.TcpPort + " / UDP " + $settings.UdpPort + "，目标 Sunshine：" + $settings.Target)

    # ---- 3. Sunshine 网页账号 -------------------------------------------
    Log '[3/6] 设置 Sunshine 网页管理账号…'
    $credFile = Join-Path $sunshineDir 'config\credentials'
    $webPasswordSaved = $WebPassword
    if (-not $SkipSunshineCreds) {
        $needCreds = $ResetWebPassword -or (-not (Test-Path -LiteralPath $credFile))
        if ($needCreds) {
            if (-not $WebPassword) {
                $chars = 'ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnpqrstuvwxyz23456789'
                $rand = -join (1..10 | ForEach-Object { $chars[(Get-Random -Maximum $chars.Length)] })
                $WebPassword = 'MiniStream-' + $rand
            }
            $webPasswordSaved = $WebPassword
            $p = Start-Process -FilePath $sunshineExe -ArgumentList @('--creds', $WebUser, $WebPassword) `
                -WorkingDirectory $sunshineDir -WindowStyle Hidden -PassThru -Wait
            if ($p.ExitCode -ne 0) {
                throw "设置 Sunshine 网页账号失败（退出码 $($p.ExitCode)）"
            }
            Log "  已设置网页账号：$WebUser（口令见安装信息.txt）"
        } else {
            $webPasswordSaved = ''
            Log '  检测到已有 Sunshine 账号，保持不变（忘记口令可用 -ResetWebPassword 重设）'
        }
    } else {
        Log '  跳过网页账号设置（-SkipSunshineCreds）'
    }

    $infoPath = Join-Path $TargetDir '安装信息.txt'
    $cfgRaw = Read-MslinkIni -Path $settings.IniPath
    $lineMainText = [string]$cfgRaw['line_main']
    $lineBackupText = [string]$cfgRaw['line_backup']
    if ($webPasswordSaved -or -not (Test-Path -LiteralPath $infoPath)) {
        $info = @"
MiniStream 被控端 安装信息
========================================
安装目录：$TargetDir
安装时间：$(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')

Sunshine 管理页面：https://localhost:47990
Sunshine 用户名：$WebUser
Sunshine 网页口令：$($webPasswordSaved)

隧道口令（远控端要填一样的）：$($settings.Password)
隧道本机监听：TCP $($settings.TcpPort) / UDP $($settings.UdpPort)

线路（FRP 公网，供核对）：
  主线：$lineMainText
  备线：$lineBackupText

自启任务：$TaskName（隧道）、$SunshineTaskName（Sunshine）
防火墙规则：$FirewallPrefix TCP / $FirewallPrefix UDP

第一次配对：
  1. 浏览器打开 https://localhost:47990 ，用上面的账号口令登录
     （提示证书不受信任时选“继续访问”，这是本机自签名证书）
  2. 远控端在 Moonlight 里添加主机 127.0.0.1，会显示 4 位 PIN
  3. 回到 Sunshine 网页，在 PIN 输入框填入这 4 位数字，点发送

改配置：用管理员身份编辑本目录 mslink-host.ini，再运行一次 安装.bat。
"@
        Write-Utf8BomFile -Path $infoPath -Text ($info -replace "`r?`n", "`r`n")
        Log '  已生成 安装信息.txt'
    } else {
        Log '  安装信息.txt 已存在，保留原文件'
    }
    Protect-MiniStreamFile -Path $infoPath -UserId $installUserId

    # ---- 4. 防火墙 -------------------------------------------------------
    Log '[4/6] 配置防火墙（只放行 1 TCP + 1 UDP）…'
    Set-FirewallRules -Tcp $settings.TcpPort -Udp $settings.UdpPort -Prefix $FirewallPrefix

    # ---- 5. 计划任务 -----------------------------------------------------
    Log '[5/6] 注册登录自启计划任务…'
    $userId = [System.Security.Principal.WindowsIdentity]::GetCurrent().Name
    $psExe = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
    $runTunnelScript = Join-Path $TargetDir '_internal\run-tunnel.ps1'
    $tunnelArgs = '-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "' + $runTunnelScript + '"'
    $tunnelXml = New-TaskXml -Name $TaskName -Command $psExe -Arguments $tunnelArgs `
        -WorkingDirectory $TargetDir -Description 'MiniStream 被控端隧道（mslink host）' `
        -UserId $userId -Hidden $true -Delay 'PT10S'
    Register-TaskFromXml -Name $TaskName -XmlText $tunnelXml

    $runSunshineScript = Join-Path $TargetDir '_internal\run-sunshine.ps1'
    $sunArgs = '-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "' + $runSunshineScript + '"'
    $sunXml = New-TaskXml -Name $SunshineTaskName -Command $psExe -Arguments $sunArgs `
        -WorkingDirectory $TargetDir -Description 'MiniStream 被控端 Sunshine 串流服务端（单实例启动器）' `
        -UserId $userId -Hidden $true -Delay 'PT5S'
    Register-TaskFromXml -Name $SunshineTaskName -XmlText $sunXml

    # ---- 6. 启动并自检 ---------------------------------------------------
    Log '[6/6] 启动并自检…'
    if (-not $SkipSunshineLaunch) {
        # 单实例检查：Sunshine 便携版可能重复启动并同时 bind 0.0.0.0:47989，
        # 表现为 serverinfo 时通时不通 / 配对随机失败；已有实例就复用，绝不起第二个。
        $existingSun = @(Get-Process -Name 'sunshine' -ErrorAction SilentlyContinue)
        $sunPortBusy = Test-TcpPortListening -Port 47989
        if ($existingSun.Count -gt 0 -or $sunPortBusy) {
            $existingPids = @($existingSun | ForEach-Object { $_.Id }) -join ','
            if (-not $existingPids) { $existingPids = '未知' }
            Log ("  检测到已有 Sunshine 实例（pid=" + $existingPids + "，47989 监听=" + $sunPortBusy + "），复用，不再启动第二个")
        } else {
            $code = Invoke-QuietCommand ('schtasks /run /tn "' + $SunshineTaskName + '"')
            if ($code -ne 0) { throw "启动 Sunshine 计划任务失败（$SunshineTaskName）" }
            Log '  已触发 Sunshine 启动（启动器内含单实例检查）'
        }
    }
    if (-not $SkipTunnelStart) {
        $code = Invoke-QuietCommand ('schtasks /run /tn "' + $TaskName + '"')
        if ($code -ne 0) { throw "启动隧道计划任务失败（$TaskName）" }

        if (-not (Wait-TunnelReady -Tcp $settings.TcpPort -Udp $settings.UdpPort -TimeoutSeconds 15)) {
            throw "隧道没有在预期时间内监听 TCP $($settings.TcpPort) / UDP $($settings.UdpPort)。请检查端口是否被别的程序占用。"
        }
        Log "  隧道已在 TCP $($settings.TcpPort) / UDP $($settings.UdpPort) 监听"
    }

    Start-Sleep -Seconds 2
    $sunAll = @(Get-Process -Name 'sunshine' -ErrorAction SilentlyContinue)
    if ($sunAll.Count -gt 0) {
        Log ("  Sunshine 进程在运行（pid=" + (@($sunAll | ForEach-Object { $_.Id }) -join ',') + "）")
    } elseif (-not $SkipSunshineLaunch) {
        Log '  [警告] 暂未检测到 Sunshine 进程，可能还在启动，稍后可在任务计划里查看'
    }
    if ($sunAll.Count -gt 1) {
        Log ("  [警告] 检测到 " + $sunAll.Count + " 个 sunshine.exe 进程（pid=" + (@($sunAll | ForEach-Object { $_.Id }) -join ',') + "）。" +
            'Sunshine 便携版可能重复启动，会导致 serverinfo 时通时不通/配对失败；请结束多余实例。')
    }

    Log ''
    Log '============================================================'
    Log ' 安装完成'
    Log "  安装目录：$TargetDir"
    Log "  隧道：TCP $($settings.TcpPort) / UDP $($settings.UdpPort)，口令不写入日志"
    if ($webPasswordSaved) {
        Log "  Sunshine 网页：https://localhost:47990  用户名 $WebUser（口令见权限受限的安装信息.txt）"
    } else {
        Log '  Sunshine 网页：https://localhost:47990（账号见首次安装生成的 安装信息.txt）'
    }
    Log '  自启任务和防火墙规则已就绪，重启后自动恢复。'
    Log '  下一步：浏览器登录 Sunshine 网页，完成 Moonlight 的 PIN 配对。'
    Log '============================================================'
    Log 'INSTALL_RESULT=OK'
    exit 0
} catch {
    Log ''
    Log ('[失败] ' + $_.Exception.Message)
    Log ('INSTALL_RESULT=FAIL: ' + $_.Exception.Message)
    exit 1
}
