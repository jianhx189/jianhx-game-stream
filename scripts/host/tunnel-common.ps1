# -*- coding: utf-8 -*-
# =============================================================================
#  tunnel-common.ps1 —— 被控端脚本共用的小工具
#  说明：本文件由 安装.bat / 卸载.bat / 启动隧道.bat 间接调用，不需要用户直接运行。
# =============================================================================

# 写日志：UTF-8 无 BOM、CRLF；失败不影响主流程
function Add-LogLine {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][AllowEmptyString()][string]$Text
    )
    try {
        $line = '[' + (Get-Date -Format 'yyyy-MM-dd HH:mm:ss') + '] ' + $Text + "`r`n"
        [System.IO.File]::AppendAllText($Path, $line, (New-Object System.Text.UTF8Encoding($false)))
    } catch {
        # 日志写不进去也不能影响安装/卸载本身
    }
}

# 解析 mslink-host.ini（key = value，# 或 ; 开头为注释；允许 UTF-8 BOM）
function Read-MslinkIni {
    param([Parameter(Mandatory = $true)][string]$Path)

    $cfg = @{}
    if (-not (Test-Path -LiteralPath $Path)) { return $cfg }

    foreach ($raw in (Get-Content -LiteralPath $Path -Encoding UTF8)) {
        $line = [string]$raw
        if ($line.Length -gt 0 -and [int][char]$line[0] -eq 0xFEFF) {
            $line = $line.Substring(1)
        }
        $line = $line.Trim()
        if ($line.Length -eq 0) { continue }
        if ($line.StartsWith(';') -or $line.StartsWith('#') -or $line.StartsWith('[')) { continue }

        $eq = $line.IndexOf('=')
        if ($eq -le 0) { continue }

        $key = $line.Substring(0, $eq).Trim().ToLowerInvariant()
        $val = $line.Substring($eq + 1).Trim()

        # 行尾注释（与远控端 mslink.ini 的解析规则保持一致）
        $hash = $val.IndexOf('#')
        if ($hash -ge 0) { $val = $val.Substring(0, $hash).Trim() }

        $cfg[$key] = $val
    }
    return $cfg
}

# 读配置并做合法性校验，返回强类型对象
function Get-MslinkHostSettings {
    param([Parameter(Mandatory = $true)][string]$Root)

    $ini = Join-Path $Root 'mslink-host.ini'
    if (-not (Test-Path -LiteralPath $ini)) {
        throw "找不到配置文件：$ini"
    }

    $cfg = Read-MslinkIni -Path $ini

    $tcpText = [string]$cfg['tcp_port']
    $udpText = [string]$cfg['udp_port']
    $tcp = 0
    $udp = 0
    if (-not [int]::TryParse($tcpText, [ref]$tcp) -or $tcp -le 0 -or $tcp -gt 65535) {
        throw "mslink-host.ini 里的 tcp_port 不合法：'$tcpText'（应为 1-65535）"
    }
    if (-not [int]::TryParse($udpText, [ref]$udp) -or $udp -le 0 -or $udp -gt 65535) {
        throw "mslink-host.ini 里的 udp_port 不合法：'$udpText'（应为 1-65535）"
    }
    if ($tcp -eq $udp) {
        throw "tcp_port 和 udp_port 不能相同（$tcp）"
    }

    $password = [string]$cfg['password']
    if (-not $password) {
        throw 'mslink-host.ini 里缺少 password（远控端要填同样的口令）'
    }
    # 口令会进入计划任务的命令行，限制在安全字符集，避免引号/空格/XML 转义问题
    if ($password -notmatch '^[A-Za-z0-9_.@-]{1,64}$') {
        throw 'password 只支持字母、数字和 _ . @ - ，长度 1-64（不要用空格、引号、中文）'
    }

    $target = [string]$cfg['target']
    if (-not $target) { $target = '127.0.0.1' }

    $bind = [string]$cfg['bind']
    if (-not $bind) { $bind = '0.0.0.0' }

    return [pscustomobject]@{
        IniPath  = $ini
        TcpPort  = $tcp
        UdpPort  = $udp
        Password = $password
        Target   = $target
        Bind     = $bind
        Root     = $Root
    }
}

# 生成 mslink host 的参数数组（顺序固定，便于计划任务/自检核对）
function Get-MslinkHostArguments {
    param([Parameter(Mandatory = $true)]$Settings)

    return @(
        'host',
        '--tcp', [string]$Settings.TcpPort,
        '--udp', [string]$Settings.UdpPort,
        '--password', [string]$Settings.Password,
        '--target', [string]$Settings.Target,
        '--bind', [string]$Settings.Bind
    )
}

# 以 UTF-8 带 BOM 写文本（PowerShell 5.1 / 记事本都能正确识别中文）
function Write-Utf8BomFile {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][AllowEmptyString()][string]$Text
    )
    [System.IO.File]::WriteAllText($Path, $Text, (New-Object System.Text.UTF8Encoding($true)))
}

# 端口是否已在监听（用于“隧道已在运行”的友好提示）
function Test-TcpPortListening {
    param([Parameter(Mandatory = $true)][int]$Port)
    try {
        $c = Get-NetTCPConnection -LocalPort $Port -State Listen -ErrorAction Stop
        return ($null -ne $c)
    } catch {
        return $false
    }
}

# 静默执行一条 cmd 命令（结果码返回给调用方）
# 用途：schtasks / netsh 这类工具在“任务/规则不存在”时会往 stderr 写错误；
# PowerShell 5.1 在 $ErrorActionPreference='Stop' 下把 stderr 重定向当异常抛出，
# 所以统一交给 cmd 做 >nul 2>&1，避免误报。
function Invoke-QuietCommand {
    param([Parameter(Mandatory = $true)][AllowEmptyString()][string]$CommandLine)
    & cmd.exe /c ($CommandLine + ' >nul 2>&1') | Out-Null
    return $LASTEXITCODE
}