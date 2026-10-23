<#
=============================================================================
 collect_ffmpeg_dlls.ps1

 Builds a MINIMAL runtime DLL set for MiniStream and installs it into
   <repo>\third_party\ffmpeg\{include,lib,bin}

 Why this script exists:
   A shared FFmpeg build pulls in ~80 DLLs (x264/x265/vpx/dav1d/glib/cairo...).
   Shipping the whole bin/ folder is wasteful and easy to get wrong.
   This walks the import tables of the 4 DLLs we actually link against,
   transitively, and copies only what is reachable.

 Requirements:
   - dumpbin.exe on PATH (comes from a VS developer prompt / vcvars64.bat)
   - a directory holding the MSYS2 ucrt64 (or similar) FFmpeg bin/lib/include

 Usage (from anywhere):
   powershell -ExecutionPolicy Bypass -File scripts\collect_ffmpeg_dlls.ps1
   powershell ... -Source D:\msys64\ucrt64 -VcVars "C:\...\vcvars64.bat"

 NOTE: keep this file ASCII-only. Windows PowerShell 5.1 reads .ps1 files
       without a BOM using the system ANSI codepage, so non-ASCII literals
       (e.g. a non-English install path) would be silently mangled.
=============================================================================
#>
param(
    # MSYS2 ucrt64 root (must contain bin\, include\, lib\)
    [string]$Source = 'D:\msys64\ucrt64',
    # vcvars64.bat used to put dumpbin on PATH
    [string]$VcVars = 'D:\VSBuildTools\VC\Auxiliary\Build\vcvars64.bat'
)

$ErrorActionPreference = 'Stop'

$repo    = Split-Path -Parent $PSScriptRoot          # <repo>
$ffRoot  = Join-Path $repo 'third_party\ffmpeg'
$srcBin  = Join-Path $Source 'bin'
$srcInc  = Join-Path $Source 'include'
$srcLib  = Join-Path $Source 'lib'
$dstBin  = Join-Path $ffRoot 'bin'
$dstInc  = Join-Path $ffRoot 'include'
$dstLib  = Join-Path $ffRoot 'lib'

Write-Host "repo    : $repo"
Write-Host "source  : $Source"

if (-not (Test-Path $srcBin)) { throw "source bin not found: $srcBin" }
if (-not (Test-Path $srcInc)) { throw "source include not found: $srcInc" }
if (-not (Test-Path $srcLib)) { throw "source lib not found: $srcLib" }

# --- locate dumpbin -------------------------------------------------------
$dumpbin = $null
if (Get-Command dumpbin.exe -ErrorAction SilentlyContinue) {
    $dumpbin = 'dumpbin.exe'   # already on PATH
} elseif (Test-Path $VcVars) {
    Write-Host "loading VS environment from $VcVars"
    $envDump = & cmd /c "call `"$VcVars`" >nul 2>&1 && where dumpbin"
    if ($LASTEXITCODE -eq 0 -and $envDump) { $dumpbin = ($envDump | Select-Object -First 1).Trim() }
}
if (-not $dumpbin) { throw "dumpbin.exe not found. Pass -VcVars pointing at vcvars64.bat." }
Write-Host "dumpbin : $dumpbin"

# --- seed: the libraries MiniStream actually links -------------------------
$seedNames = @('avcodec*', 'avutil*', 'swscale*', 'swresample*')
$seed = @()
foreach ($p in $seedNames) {
    $seed += Get-ChildItem (Join-Path $srcBin $p) -ErrorAction SilentlyContinue |
             Where-Object { $_.Extension -eq '.dll' } |
             Select-Object -ExpandProperty Name
}
$seed = $seed | Sort-Object -Unique
if ($seed.Count -eq 0) { throw "no FFmpeg DLLs found in $srcBin" }
Write-Host ("seed DLLs: " + ($seed -join ', '))

# --- transitive closure via dumpbin /dependents ----------------------------
$seen = @{}
foreach ($s in $seed) { $seen[$s.ToLower()] = $true }
$pending = New-Object System.Collections.ArrayList
foreach ($s in $seed) { [void]$pending.Add($s) }

$all = @{}      # file -> list of deps
$round = 0
while ($pending.Count -gt 0 -and $round -lt 20) {
    $round++
    $names = @($pending)
    $argList = @('/nologo', '/dependents')
    foreach ($n in $names) { $argList += (Join-Path $srcBin $n) }

    $out = & $dumpbin @argList 2>&1
    $cur = ''
    foreach ($line in $out) {
        if ($line -match 'Dump of file (.+)$') {
            $cur = (Split-Path -Leaf $matches[1]).ToLower()
            if (-not $all.ContainsKey($cur)) { $all[$cur] = New-Object System.Collections.ArrayList }
            continue
        }
        if ($line -match '^\s*(\S+\.dll)\s*$') {
            $dep = $matches[1].ToLower()
            if ($cur) { [void]$all[$cur].Add($dep) }
        }
    }

    $next = New-Object System.Collections.ArrayList
    foreach ($f in $names) {
        $key = $f.ToLower()
        if (-not $all.ContainsKey($key)) { continue }
        foreach ($d in $all[$key]) {
            if ($seen.ContainsKey($d)) { continue }
            if (-not (Test-Path (Join-Path $srcBin $d))) { continue }   # system DLL, skip
            $seen[$d] = $true
            [void]$next.Add($d)
        }
    }
    $pending = $next
    Write-Host ("  round {0}: +{1}" -f $round, $pending.Count)
}

# --- copy -----------------------------------------------------------------
foreach ($d in @($dstBin, $dstInc, $dstLib)) {
    if (Test-Path $d) { Remove-Item (Join-Path $d '*') -Recurse -Force -ErrorAction SilentlyContinue }
    New-Item -ItemType Directory -Force -Path $d | Out-Null
}

$copied = 0
foreach ($name in $seen.Keys) {
    $p = Get-ChildItem (Join-Path $srcBin $name) -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($p) { Copy-Item $p.FullName $dstBin -Force; $copied++ }
}
Write-Host "runtime DLLs copied: $copied"

# headers
foreach ($h in @('libav*', 'libsw*')) {
    Get-ChildItem (Join-Path $srcInc $h) -Directory -ErrorAction SilentlyContinue |
        ForEach-Object { Copy-Item $_.FullName $dstInc -Recurse -Force }
}

# import libraries: keep the GNU .dll.a AND make an MSVC-style .lib copy.
# MSVC's link.exe accepts the COFF import library produced by GNU ld, so a
# plain rename is enough -- no need for gyan.dev's MSVC-specific package.
# Only the 7 libraries we actually link are copied; the MSYS2 lib/ folder has
# hundreds of unrelated import libs (llama.cpp, OpenEXR, python...) we do not want.
$libNames = @('avcodec', 'avutil', 'swscale', 'swresample', 'avformat', 'avfilter', 'avdevice')
foreach ($n in $libNames) {
    $a = Join-Path $srcLib ("lib{0}.dll.a" -f $n)
    if (Test-Path $a) {
        Copy-Item $a $dstLib -Force
        Copy-Item $a (Join-Path $dstLib ("{0}.lib" -f $n)) -Force
    }
}
Write-Host "headers + import libs installed to $dstInc / $dstLib"

# --- report ---------------------------------------------------------------
Write-Host ""
Write-Host "=== installed ==="
Get-ChildItem $dstBin -Filter '*.dll' | Measure-Object | ForEach-Object { Write-Host ("  bin  : {0} DLLs" -f $_.Count) }
Get-ChildItem $dstLib -Filter '*.lib' | ForEach-Object { Write-Host ("  lib  : {0}" -f $_.Name) }
Write-Host ""
Write-Host "Done. Now configure with -DFFMPEG_ROOT=$ffRoot"
