param([Parameter(Mandatory=$true)][string]$Root)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'tunnel-common.ps1')
$Root = [IO.Path]::GetFullPath($Root).TrimEnd('\')

function Get-ListenerState($Endpoints, [string]$RelativePath) {
    $expected = Join-Path $Root $RelativePath
    $expectedName = [IO.Path]::GetFileNameWithoutExtension($RelativePath)
    $unknown = $false
    foreach ($endpoint in $Endpoints) {
        $process = Get-Process -Id $endpoint.OwningProcess -ErrorAction SilentlyContinue
        if ($process.Path -ieq $expected) { return 'ready' }
        if (-not $process.Path) {
            $info = Get-CimInstance Win32_Process -Filter "ProcessId=$($endpoint.OwningProcess)" -ErrorAction SilentlyContinue
            if ($info.ExecutablePath -ieq $expected) { return 'ready' }
            if (-not $info.ExecutablePath -and $process.ProcessName -ieq $expectedName) {
                if ($expectedName -ieq 'sunshine') { return 'limited' }
                # Login tasks may run elevated. Verify socket ownership against fresh telemetry in this installation.
                try {
                    $statusPath = Join-Path $Root 'mslink-status.json'
                    $file = Get-Item -LiteralPath $statusPath
                    if ($file.Length -le 65536) {
                        $state = [IO.File]::ReadAllText($statusPath) | ConvertFrom-Json
                        $age = [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds() - [double]$state.updated_unix_ms
                        if ($state.schema -eq 1 -and $state.role -eq 'host' -and
                            $state.pid -eq $endpoint.OwningProcess -and $age -ge 0 -and $age -lt 5000) { return 'limited' }
                    }
                } catch {}
            }
            if (-not $info.ExecutablePath) { $unknown = $true }
        }
    }
    if ($unknown) { return 'unknown' }
    if ($Endpoints.Count -gt 0) { return 'occupied' }
    return 'missing'
}

function Test-ManagedRule($Rules, [string]$Protocol, [int]$Port) {
    foreach ($rule in $Rules) {
        if ($rule.DisplayName -ne "MiniStream Tunnel $Protocol" -or
            $rule.Enabled -ne 'True' -or $rule.Direction -ne 'Inbound' -or
            $rule.Action -ne 'Allow' -or "$($rule.Profile)" -ne 'Any') { continue }
        $filter = $rule | Get-NetFirewallPortFilter
        $app = $rule | Get-NetFirewallApplicationFilter
        if ($filter.Protocol -eq $Protocol -and @($filter.LocalPort) -contains [string]$Port -and
            ($app.Program -eq 'Any' -or [Environment]::ExpandEnvironmentVariables($app.Program) -ieq (Join-Path $Root 'mslink.exe'))) {
            return 'ready'
        }
    }
    return 'missing'
}

$result = [ordered]@{
    config='invalid'; sunshine='unknown'; http='unknown'; tcp='unknown'; udp='unknown'
    firewallTcp='unknown'; firewallUdp='unknown'; policy='unknown'; startup='unknown'
}
try {
    $result.sunshine = Get-ListenerState @(Get-NetTCPConnection -State Listen | Where-Object LocalPort -eq 47989) 'sunshine\sunshine.exe'
    if ($result.sunshine -in 'ready','limited') {
        $result.http = 'failed'
        $request = [Net.HttpWebRequest]::Create('http://127.0.0.1:47989/serverinfo?uniqueid=0123456789ABCDEF')
        $request.Proxy = $null
        $request.AllowAutoRedirect = $false
        $request.Timeout = 2500
        $request.ReadWriteTimeout = 2500
        $response = $request.GetResponse()
        try {
            $reader = New-Object IO.StreamReader($response.GetResponseStream())
            try {
                $buffer = New-Object char[] 4096
                $count = $reader.ReadBlock($buffer, 0, $buffer.Length)
                $body = -join ($buffer | Select-Object -First $count)
                if ($body -match '<root\b[^>]*\bstatus_code="200"' -and $body -match '<hostname>') { $result.http = 'ready' }
            } finally { $reader.Dispose() }
        } finally { $response.Dispose() }
    }
} catch {}

try {
    $settings = Get-MslinkHostSettings -Root $Root -WarningAction SilentlyContinue
    $result.config = 'ready'
    $result.tcpPort = $settings.TcpPort
    $result.udpPort = $settings.UdpPort
} catch {}
if ($result.config -eq 'ready') {
    try { $result.tcp = Get-ListenerState @(Get-NetTCPConnection -State Listen | Where-Object LocalPort -eq $settings.TcpPort) 'mslink.exe' } catch {}
    try { $result.udp = Get-ListenerState @(Get-NetUDPEndpoint | Where-Object LocalPort -eq $settings.UdpPort) 'mslink.exe' } catch {}
    try {
        $rules = @(Get-NetFirewallRule -PolicyStore ActiveStore | Where-Object DisplayName -in @('MiniStream Tunnel TCP','MiniStream Tunnel UDP'))
        $result.firewallTcp = Test-ManagedRule $rules 'TCP' $settings.TcpPort
        $result.firewallUdp = Test-ManagedRule $rules 'UDP' $settings.UdpPort
    } catch {}
}
try {
    $names = @(Get-NetConnectionProfile | ForEach-Object {
        if ($_.NetworkCategory -eq 'DomainAuthenticated') { 'Domain' } else { [string]$_.NetworkCategory }
    } | Select-Object -Unique)
    if ($names.Count -gt 0) {
        $profiles = @(Get-NetFirewallProfile -PolicyStore ActiveStore | Where-Object Name -in $names)
        if (@($profiles | Where-Object Enabled -eq 'False').Count -gt 0) { $result.policy='disabled' }
        elseif (@($profiles | Where-Object { $_.AllowInboundRules -eq 'False' -or $_.AllowLocalFirewallRules -eq 'False' }).Count -gt 0) { $result.policy='restricted' }
        elseif ($profiles.Count -eq $names.Count) { $result.policy='ready' }
    }
} catch {}
try {
    $taskNames = @('MiniStreamTunnel','MiniStreamSunshine')
    $tasks = @(Get-ScheduledTask | Where-Object TaskName -in $taskNames)
    $enabledTasks = @($tasks | Where-Object {
        $exe = if ($_.TaskName -eq 'MiniStreamTunnel') { 'run-tunnel.ps1' } else { 'run-sunshine.ps1' }
        $script = Join-Path (Join-Path $Root '_internal') $exe
        $_.State -ne 'Disabled' -and $_.Actions.Arguments -like ('*"' + [WildcardPattern]::Escape($script) + '"*')
    })
    $result.startup = if ($enabledTasks.Count -eq 2) { 'ready' } else { 'missing' }
} catch {}
$result | ConvertTo-Json -Compress
