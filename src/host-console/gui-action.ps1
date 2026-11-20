param(
    [Parameter(Mandatory=$true)][string]$Root,
    [ValidateSet('start','stop','restart','save','password','firewall')][string]$Action,
    [ValidateSet('sunshine','tunnel','all')][string]$Component,
    [ValidateRange(1,65535)][int]$TcpPort=14738,
    [ValidateRange(1,65535)][int]$UdpPort=14739,
    [ValidateRange(1000,120000)][int]$TimeoutMs=10000
)
$ErrorActionPreference = 'Stop'
$Root = [IO.Path]::GetFullPath($Root).TrimEnd('\')
if (-not (Test-Path -LiteralPath (Join-Path $Root 'mslink.exe')) -or
    -not (Test-Path -LiteralPath (Join-Path $Root 'sunshine\sunshine.exe'))) { exit 2 }

. (Join-Path $PSScriptRoot 'tunnel-common.ps1')
$log = Join-Path $Root 'gui-action.log'

function Set-SettingText([string]$Text, [hashtable]$Values) {
    foreach ($key in $Values.Keys) {
        $pattern = '(?im)^[ \t]*' + [regex]::Escape($key) + '[ \t]*=[^\r\n]*'
        $line = $key + ' = ' + $Values[$key]
        if ([regex]::IsMatch($Text, $pattern)) {
            $Text = [regex]::Replace($Text, $pattern, [Text.RegularExpressions.MatchEvaluator]{ param($m) $line })
        } else {
            $Text += "`r`n$line`r`n"
        }
    }
    return $Text
}

function Write-ProtectedReplacement([string]$Path, [byte[]]$Bytes) {
    $temp = Join-Path (Split-Path -Parent $Path) ([Guid]::NewGuid().ToString() + '.tmp')
    $acl = [IO.File]::GetAccessControl($Path, [Security.AccessControl.AccessControlSections]::Access)
    try {
        $stream = [IO.File]::Open($temp, [IO.FileMode]::CreateNew)
        $stream.Dispose()
        [IO.File]::SetAccessControl($temp, $acl)
        [IO.File]::WriteAllBytes($temp, $Bytes)
        # File.Replace requires a null, not an empty PowerShell string, for no backup.
        [IO.File]::Replace($temp, $Path, [NullString]::Value)
    } finally {
        if ([IO.File]::Exists($temp)) { [IO.File]::Delete($temp) }
    }
}

function Get-ManagedProcess([string]$Name, [string]$RelativePath) {
    $expected = Join-Path $Root $RelativePath
    Get-CimInstance Win32_Process -Filter "Name='$Name'" | Where-Object {
        $_.ExecutablePath -and [IO.Path]::GetFullPath($_.ExecutablePath) -ieq $expected
    }
}

function Stop-ManagedProcess($Processes) {
    foreach ($process in $Processes) {
        $live = Get-Process -Id $process.ProcessId -ErrorAction SilentlyContinue
        if (-not $live) { continue }
        if (-not $live.Path -or $live.Path -ine $process.ExecutablePath) { throw 'Process identity changed' }
        Stop-Process -InputObject $live -Force
        if (-not $live.WaitForExit(5000)) { throw 'Service did not stop' }
    }
}

function Start-CheckedTunnel($Settings) {
    $existing = @(Get-ManagedProcess 'mslink.exe' 'mslink.exe')
    if ($existing.Count -gt 1) { throw 'More than one managed tunnel process' }
    if ($existing.Count -eq 1) {
        $pidToCheck = $existing[0].ProcessId
        $proc = Get-Process -Id $pidToCheck
    } else {
        $proc = Start-MslinkTunnel -Settings $Settings
        $pidToCheck = $proc.Id
    }
    try {
        $deadline = [DateTime]::UtcNow.AddSeconds(12)
        do {
            if ($proc.HasExited) { throw 'Tunnel exited before opening its ports' }
            $tcp = Get-NetTCPConnection -State Listen -LocalPort $Settings.TcpPort -ErrorAction SilentlyContinue |
                Where-Object OwningProcess -eq $pidToCheck
            $udp = Get-NetUDPEndpoint -LocalPort $Settings.UdpPort -ErrorAction SilentlyContinue |
                Where-Object OwningProcess -eq $pidToCheck
            if ($tcp -and $udp) { return }
            Start-Sleep -Milliseconds 250
        } while ([DateTime]::UtcNow -lt $deadline)
        throw 'Managed tunnel did not own both configured listeners'
    } catch {
        if ($existing.Count -eq 0 -and -not $proc.HasExited) { $proc.Kill(); [void]$proc.WaitForExit(5000) }
        throw
    } finally {
        $proc.Dispose()
    }
}

function Update-TunnelPassword {
    $password = [Console]::In.ReadLine()
    if ($password -cnotmatch '\A[A-Za-z0-9_.@-]{12,64}\z') { throw 'Invalid tunnel password format' }
    $settings = Get-MslinkHostSettings -Root $Root -WarningAction SilentlyContinue
    if ($password -ceq $settings.Password) { return }
    $original = [IO.File]::ReadAllBytes($settings.IniPath)
    $updated = Set-SettingText ([IO.File]::ReadAllText($settings.IniPath)) @{password=$password}
    $running = @(Get-ManagedProcess 'mslink.exe' 'mslink.exe')
    if ($running.Count -gt 1) { throw 'More than one managed tunnel process' }
    Write-ProtectedReplacement $settings.IniPath ([Text.Encoding]::UTF8.GetBytes($updated))
    if ($running.Count -eq 0) { return }
    try {
        Stop-ManagedProcess $running
        Start-CheckedTunnel (Get-MslinkHostSettings -Root $Root)
    } catch {
        try {
            Write-ProtectedReplacement $settings.IniPath $original
            Start-CheckedTunnel $settings
            Add-LogLine -Path $log -Text 'Password rotation failed. Previous configuration restored; both tunnel listeners verified.'
            return 3
        } catch {
            Add-LogLine -Path $log -Text 'Password rotation failed and restoration could not be verified.'
            return 4
        }
    }
    return 0
}

function Save-PortsAndTimeout {
    if ($TcpPort -eq $UdpPort) { throw 'TCP and UDP ports must differ' }
    $ini = Join-Path $Root 'mslink-host.ini'
    $sunshine = Join-Path $Root 'sunshine\config\sunshine.conf'
    $original = [IO.File]::ReadAllBytes($ini)
    $nextIni = Set-SettingText ([IO.File]::ReadAllText($ini)) @{tcp_port=$TcpPort;udp_port=$UdpPort}
    $nextSunshine = Set-SettingText ([IO.File]::ReadAllText($sunshine)) @{ping_timeout=$TimeoutMs}
    Write-ProtectedReplacement $ini ([Text.Encoding]::UTF8.GetBytes($nextIni))
    try {
        Write-ProtectedReplacement $sunshine ([Text.Encoding]::UTF8.GetBytes($nextSunshine))
    } catch {
        Write-ProtectedReplacement $ini $original
        throw
    }
}

function Repair-Firewall {
    $settings = Get-MslinkHostSettings -Root $Root
    $program = Join-Path $Root 'mslink.exe'
    foreach ($protocol in @('TCP', 'UDP')) {
        $port = if ($protocol -eq 'TCP') { $settings.TcpPort } else { $settings.UdpPort }
        $name = 'MiniStream Tunnel ' + $protocol
        $values = @{Direction='Inbound'; Action='Allow'; Enabled='True'; Profile='Any'
                    Protocol=$protocol; LocalPort=$port; Program=$program}
        $rules = @(Get-NetFirewallRule -DisplayName $name -ErrorAction SilentlyContinue)
        if ($rules.Count -eq 0) { New-NetFirewallRule -DisplayName $name @values | Out-Null }
        else { $rules | Set-NetFirewallRule @values | Out-Null }
    }
}

$mutex = $null
$owned = $false
try {
    $mutex = New-Object Threading.Mutex($false, 'Global\MiniStreamHostConsoleAction')
    try { $owned = $mutex.WaitOne(0) } catch [Threading.AbandonedMutexException] { $owned = $true }
    if (-not $owned) { throw 'Another MiniStream host operation is running' }

    switch ($Action) {
        'password' {
            $code = Update-TunnelPassword
            if ($code) { exit [int]$code }
        }
        'save' { Save-PortsAndTimeout }
        'firewall' { Repair-Firewall }
        default {
            if ($Component -eq 'all') { throw 'Choose a service' }
            $sunshine = $Component -eq 'sunshine'
            $name = if ($sunshine) { 'sunshine.exe' } else { 'mslink.exe' }
            $file = if ($sunshine) { 'sunshine\sunshine.exe' } else { 'mslink.exe' }
            if ($Action -in 'stop','restart') { Stop-ManagedProcess @(Get-ManagedProcess $name $file) }
            if ($Action -in 'start','restart') {
                if ($sunshine) {
                    & (Join-Path $PSScriptRoot 'run-sunshine.ps1') -TargetDir $Root
                    if ($LASTEXITCODE) { throw 'Sunshine startup failed' }
                } else {
                    Start-CheckedTunnel (Get-MslinkHostSettings -Root $Root)
                }
            }
        }
    }
    Add-LogLine -Path $log -Text "$Action $Component completed"
    exit 0
} catch {
    # Config/credential exceptions can carry input text. Keep this log free of request data.
    Add-LogLine -Path $log -Text "$Action $Component failed ($($_.Exception.GetType().Name), line $($_.InvocationInfo.ScriptLineNumber))."
    exit 1
} finally {
    if ($owned) { $mutex.ReleaseMutex() }
    if ($mutex) { $mutex.Dispose() }
}
