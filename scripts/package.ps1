# =============================================================================
#  package.ps1 - build the user-facing delivery zip
#
#  Layout produced (same as previous deliveries):
#     MiniStream-<deliver>/
#       MiniStream-<host>/     ministream-host.exe + videoprobe + selftest + DLLs
#                             host.ini + user guide
#       MiniStream-<client>/   ministream-client.exe + DLLs + client.ini + user guide
#       <tools>/               minidump_info.py + stack_code_frames.py + readme
#       README.md, BUILD-<...>.md, PROTOCOL-<...>.md
#     and a zip next to the workspace.
#
#  ASCII only: Windows PowerShell 5.1 reads .ps1 as ANSI unless it has a BOM.
#  Chinese names are built from code points so this file stays pure ASCII.
# =============================================================================
param(
    [switch]$SkipZip
)

$ErrorActionPreference = 'Stop'

# --- Chinese name fragments (code points) ------------------------------------
# Every localised file/folder name used by the delivery is assembled here, so
# the rest of this script stays pure ASCII.
$N_HOST    = [string][char]0x88AB + [char]0x63A7 + [char]0x7AEF          # host side folder
$N_CLIENT  = [string][char]0x8FDC + [char]0x63A7 + [char]0x7AEF          # client side folder
$N_TOOLS   = [string][char]0x5DE5 + [char]0x5177                          # tools folder
$N_MANUAL  = [string][char]0x4F7F + [char]0x7528 + [char]0x8BF4 + [char]0x660E + '.md'  # user guide
$N_README  = [string][char]0x8BF4 + [char]0x660E + '.md'                  # tools readme
$N_DELIVER = 'MiniStream-' + [char]0x4EA4 + [char]0x4ED8                  # staging + zip name
$N_BUILD   = 'BUILD-' + [char]0x7F16 + [char]0x8BD1 + [char]0x8BF4 + [char]0x660E + '.md'
$N_PROTO   = 'PROTOCOL-' + [char]0x534F + [char]0x8BAE + [char]0x89C4 + [char]0x8303 + '.md'

$Root = Split-Path -Parent $PSScriptRoot
$Bin  = Join-Path $Root 'build\msvc\bin'
$Stage = Join-Path (Split-Path -Parent $Root) $N_DELIVER
$Zip   = Join-Path (Split-Path -Parent $Root) ($N_DELIVER + '.zip')

Write-Host "workspace : $Root"
Write-Host "binaries  : $Bin"
Write-Host "staging   : $Stage"
Write-Host "zip       : $Zip"

# --- safety: never overwrite binaries that are still running -----------------
$running = @(Get-Process ministream* -ErrorAction SilentlyContinue)
if ($running.Count -gt 0) {
    $running | Select-Object Id, ProcessName, Path | Format-Table -AutoSize
    throw "MiniStream processes are running; stop them first (they lock the exe/dll files)."
}

if (-not (Test-Path (Join-Path $Bin 'ministream-host.exe'))) {
    throw "ministream-host.exe not found in $Bin - build first (scripts\build.bat)."
}

# --- staging: recreate from scratch -----------------------------------------
$expectParent = Split-Path -Parent $Root
if ((Split-Path -Parent $Stage) -ne $expectParent -or (Split-Path -Leaf $Stage) -ne $N_DELIVER) {
    throw "refusing to clean unexpected path: $Stage"
}
if (Test-Path $Stage) {
    Write-Host "removing old staging dir: $Stage"
    Remove-Item $Stage -Recurse -Force
}
$hostDir = Join-Path $Stage ('MiniStream-' + $N_HOST)
$cliDir  = Join-Path $Stage ('MiniStream-' + $N_CLIENT)
$toolDir = Join-Path $Stage $N_TOOLS
New-Item -ItemType Directory -Force -Path $hostDir, $cliDir, $toolDir | Out-Null

# --- host payload -----------------------------------------------------------
Copy-Item (Join-Path $Bin 'ministream-host.exe')        $hostDir -Force
Copy-Item (Join-Path $Bin 'ministream-videoprobe.exe')  $hostDir -Force
if (Test-Path (Join-Path $Bin 'ministream-selftest.exe')) {
    Copy-Item (Join-Path $Bin 'ministream-selftest.exe') $hostDir -Force
}
Copy-Item (Join-Path $Bin '*.dll') $hostDir -Force
Copy-Item (Join-Path $Root 'config\host.ini.example') (Join-Path $hostDir 'host.ini') -Force
Copy-Item (Join-Path $Root 'docs\USAGE.md') (Join-Path $hostDir $N_MANUAL) -Force

# --- client payload ---------------------------------------------------------
Copy-Item (Join-Path $Bin 'ministream-client.exe') $cliDir -Force
Copy-Item (Join-Path $Bin '*.dll') $cliDir -Force
Copy-Item (Join-Path $Root 'config\client.ini.example') (Join-Path $cliDir 'client.ini') -Force
Copy-Item (Join-Path $Root 'docs\USAGE.md') (Join-Path $cliDir $N_MANUAL) -Force

# --- tools ------------------------------------------------------------------
Copy-Item (Join-Path $Root 'tools\minidump_info.py')     $toolDir -Force
Copy-Item (Join-Path $Root 'tools\stack_code_frames.py') $toolDir -Force
Copy-Item (Join-Path $Root 'tools\hang_threads.py')      $toolDir -Force
Copy-Item (Join-Path $Root 'tools\dump_threads_detail.py') $toolDir -Force
Copy-Item (Join-Path $Root 'tools\pe_exports_from_dump.py') $toolDir -Force
Copy-Item (Join-Path $Root 'tools\tools-readme.md') (Join-Path $toolDir $N_README) -Force
Copy-Item (Join-Path $Root 'tools\collect-client-diag.vbs') $toolDir -Force

# --- root docs --------------------------------------------------------------
Copy-Item (Join-Path $Root 'README.md')        (Join-Path $Stage 'README.md') -Force
Copy-Item (Join-Path $Root 'docs\BUILD.md')    (Join-Path $Stage $N_BUILD) -Force
Copy-Item (Join-Path $Root 'docs\PROTOCOL.md') (Join-Path $Stage $N_PROTO) -Force
# Engineering-settings mapping doc (Moonlight / RustDesk) -- most useful when
# diagnosing stutter / corruption / transport flapping.
$N_OPTIM = 'OPTIMIZATIONS-' + [char]0x4F18 + [char]0x5316 + [char]0x8BBE + [char]0x5B9A + '.md'
Copy-Item (Join-Path $Root 'docs\OPTIMIZATIONS.md') (Join-Path $Stage $N_OPTIM) -Force

# --- summary ----------------------------------------------------------------
foreach ($d in @($hostDir, $cliDir, $toolDir)) {
    $files = Get-ChildItem $d -File
    $mb = [Math]::Round((($files | Measure-Object Length -Sum).Sum / 1MB), 1)
    Write-Host ("  {0,-28} {1,3} files  {2,7} MB" -f (Split-Path -Leaf $d), $files.Count, $mb)
}

# --- zip --------------------------------------------------------------------
if (-not $SkipZip) {
    # Compress-Archive only accepts a .zip destination, so build a partial file
    # next to the target and move it into place when it succeeded.
    $tmp = "$Zip.part.zip"
    if (Test-Path $tmp) { Remove-Item $tmp -Force }
    if (Test-Path $Zip) { Remove-Item $Zip -Force }
    Write-Host "compressing ..."
    Compress-Archive -Path (Join-Path $Stage '*') -DestinationPath $tmp -CompressionLevel Optimal
    Move-Item $tmp $Zip -Force
    $z = Get-Item $Zip
    Write-Host ("zip: {0}  {1} MB" -f $z.FullName, [Math]::Round($z.Length / 1MB, 1)) -ForegroundColor Green
} else {
    Write-Host "zip skipped (-SkipZip)" -ForegroundColor DarkYellow
}

Write-Host "PACKAGE_OK" -ForegroundColor Green
