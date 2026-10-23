# =============================================================================
#  abtest.ps1 - A/B regression: the "streaming + stop service" crash scenario
#
#  Same scripted scenario against two host builds:
#    1) start host in tray mode
#    2) connect the real client and let it stream
#    3) send WM_CLOSE to the host message window  (== Stop service / Exit)
#    4) report host exit code and whether a crash dump appeared
#
#  Old build: exits 0xC0000409 (-1073740791)  -> the reported "black screen then
#             app disappeared" bug.
#  Fixed build: exits 0.
#
#  ASCII only: Windows PowerShell 5.1 reads .ps1 as ANSI unless it has a BOM.
# =============================================================================
param(
    [string]$ClientExe = '',
    [string]$OldDir    = '',
    [string]$NewDir    = '',
    [int]   $OldPort   = 28500,
    [int]   $NewPort   = 28501
)

$ErrorActionPreference = 'Stop'

# Resolve every path relative to the script location: this file must stay ASCII
# (Windows PowerShell 5.1 reads .ps1 as ANSI without a BOM), and the workspace
# path contains non-ASCII characters.
$Root = Split-Path -Parent $PSScriptRoot
if (-not $NewDir)    { $NewDir    = Join-Path $Root 'build\msvc\bin' }
if (-not $OldDir)    { $OldDir    = Join-Path $Root 'build\abtest\old' }
if (-not $ClientExe) { $ClientExe = Join-Path $Root 'build\msvc\bin\ministream-client.exe' }

Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public class MsWin {
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)]
    public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint msg, IntPtr wp, IntPtr lp);

    // Locate the host's hidden message window by pid + class name.
    // Do NOT use FindWindowW(class, null): PowerShell marshals $null for a
    // [string] parameter as an empty string, i.e. it would search for a window
    // with an empty title and always return 0.
    public static IntPtr FindInProcess(string cls, uint pid) {
        IntPtr found = IntPtr.Zero;
        EnumWindows((h, l) => {
            uint q; GetWindowThreadProcessId(h, out q);
            if (q == pid) {
                var sb = new StringBuilder(256);
                GetClassNameW(h, sb, 256);
                if (sb.ToString() == cls) { found = h; return false; }
            }
            return true;
        }, IntPtr.Zero);
        return found;
    }
}
"@

# "listening" marker in host.log, built from code points so this file stays ASCII
$LISTEN_MARK = [string][char]0x6B63 + [char]0x5728 + [char]0x76D1 + [char]0x542C

function Write-Ini([string]$dir, [int]$port) {
    $text = @"
[server]
port=$port
password=test123
bind=127.0.0.1

[video]
fps=30
bitrate_kbps=3000
width=0
height=0
encoder=auto
monitor=0
keyframe_interval_sec=2

[input]
enabled=true

[log]
level=2
"@
    Set-Content -Path (Join-Path $dir 'host.ini') -Value $text -Encoding ASCII
}

function Run-Scenario([string]$label, [string]$dir, [int]$port) {
    Write-Host ""
    Write-Host "================ $label ================" -ForegroundColor Cyan
    Write-Host "  dir: $dir   port: $port"

    Write-Ini $dir $port
    Remove-Item (Join-Path $dir 'host.log')    -ErrorAction SilentlyContinue
    Remove-Item (Join-Path $dir 'crash.log')   -ErrorAction SilentlyContinue
    Remove-Item (Join-Path $dir 'crash_*.dmp') -ErrorAction SilentlyContinue

    $hostExe = Join-Path $dir 'ministream-host.exe'
    $proc = Start-Process -FilePath $hostExe -WorkingDirectory $dir -PassThru

    $listening = $false
    for ($i = 0; $i -lt 60; $i++) {
        Start-Sleep -Milliseconds 250
        $log = Get-Content (Join-Path $dir 'host.log') -Encoding UTF8 -ErrorAction SilentlyContinue
        if ($log -match $LISTEN_MARK) { $listening = $true; break }
    }
    Write-Host "  listening: $listening"

    $cliArgs = @('--auto', '--host', '127.0.0.1', '--port', "$port",
                 '--password', 'test123', '--seconds', '12')
    $cli = Start-Process -FilePath $ClientExe -ArgumentList $cliArgs -PassThru
    Start-Sleep -Seconds 4

    $hwnd = [MsWin]::FindInProcess('MiniStreamHostMsgWnd', [uint32]$proc.Id)
    $winPid = 0
    if ($hwnd -ne [IntPtr]::Zero) {
        [void][MsWin]::GetWindowThreadProcessId($hwnd, [ref]$winPid)
    }
    Write-Host "  message window: hwnd=$hwnd pid=$winPid  (host pid=$($proc.Id))"
    [void][MsWin]::PostMessageW($hwnd, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)   # WM_CLOSE

    $exited = $proc.WaitForExit(15000)
    if (-not $exited) {
        Write-Host "  !! host did not exit within 15s (hung)" -ForegroundColor Red
        $proc.Kill()
    } else {
        $code = $proc.ExitCode
        $verdict = 'clean exit'
        if ($code -eq -1073740791) { $verdict = 'CRASH 0xC0000409' }
        elseif ($code -eq 3)       { $verdict = 'abort (std::terminate path)' }
        elseif ($code -ne 0)       { $verdict = 'non-zero exit' }
        $color = if ($code -eq 0) { 'Green' } else { 'Red' }
        Write-Host ("  host exit code: {0}  -> {1}" -f $code, $verdict) -ForegroundColor $color
    }

    if (-not $cli.HasExited) { $cli.Kill() }
    Start-Sleep -Milliseconds 800

    $dumps = @(Get-ChildItem (Join-Path $dir 'crash_*.dmp') -ErrorAction SilentlyContinue)
    $crashLog = Join-Path $dir 'crash.log'
    Write-Host ("  crash dumps: {0}" -f $dumps.Count)
    if (Test-Path $crashLog) {
        Write-Host "  --- crash.log ---"
        Get-Content $crashLog -Encoding UTF8 | ForEach-Object { "    $_" }
    }
    Write-Host "  --- tail of host.log ---"
    Get-Content (Join-Path $dir 'host.log') -Encoding UTF8 -ErrorAction SilentlyContinue |
        Select-Object -Last 5 | ForEach-Object { "    $_" }
}

Write-Host "MiniStream A/B regression: stop service while streaming" -ForegroundColor Yellow
Write-Host "(old build should crash with 0xC0000409, fixed build should exit cleanly)"

if (Test-Path (Join-Path $OldDir 'ministream-host.exe')) {
    Run-Scenario 'OLD build (shipped binary)' $OldDir $OldPort
} else {
    Write-Host "skipping OLD build: no ministream-host.exe in $OldDir" -ForegroundColor DarkYellow
}
Run-Scenario 'FIXED build (current)' $NewDir $NewPort

Write-Host ""
Write-Host "done." -ForegroundColor Yellow
