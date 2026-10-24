param([Parameter(Mandatory=$true)][string]$Root,[ValidateSet('start','stop','restart','save')][string]$Action,[ValidateSet('sunshine','tunnel','all')][string]$Component,[ValidateRange(1,65535)][int]$TcpPort=14738,[ValidateRange(1,65535)][int]$UdpPort=14739,[ValidateRange(1000,120000)][int]$TimeoutMs=10000)
$ErrorActionPreference='Stop'
$Root=[IO.Path]::GetFullPath($Root).TrimEnd('\')
if(-not(Test-Path -LiteralPath (Join-Path $Root 'mslink.exe')) -or -not(Test-Path -LiteralPath (Join-Path $Root 'sunshine\sunshine.exe'))){exit 2}
$log=Join-Path $Root 'gui-action.log'
function Get-UpdatedSetting([string]$Path,[hashtable]$Values){
 if(-not(Test-Path -LiteralPath $Path)){throw 'Configuration missing. Complete host installation first.'}
 $text=[IO.File]::ReadAllText($Path)
 foreach($k in $Values.Keys){$pattern='(?m)^\s*'+[regex]::Escape($k)+'\s*=.*$';$line=$k+' = '+$Values[$k];if([regex]::IsMatch($text,$pattern)){$text=[regex]::Replace($text,$pattern,$line)}else{$text+="`r`n$line`r`n"}}
 return $text
}
. (Join-Path $Root '_internal\tunnel-common.ps1')
function Save-Settings {
 $paths=@((Join-Path $Root 'mslink-host.ini'),(Join-Path $Root 'sunshine\config\sunshine.conf'))
 $values=@(@{tcp_port=$TcpPort;udp_port=$UdpPort},@{ping_timeout=$TimeoutMs})
 $changed=New-Object System.Collections.Generic.List[int]
 try {
  # Prepare both files before replacing either. Roll back an earlier replacement if a later one fails.
  for($i=0;$i -lt $paths.Count;$i++) {
   $text=Get-UpdatedSetting $paths[$i] $values[$i]
   Copy-Item -LiteralPath $paths[$i] -Destination ($paths[$i]+'.backup') -Force
   [IO.File]::WriteAllText(($paths[$i]+'.new'),$text,(New-Object Text.UTF8Encoding($false)))
  }
  for($i=0;$i -lt $paths.Count;$i++) { [IO.File]::Replace(($paths[$i]+'.new'),$paths[$i],($paths[$i]+'.backup'));$changed.Add($i) }
  $userId=[Security.Principal.WindowsIdentity]::GetCurrent().Name
  Protect-MiniStreamFile -Path $paths[0] -UserId $userId
  Protect-MiniStreamFile -Path ($paths[0]+'.backup') -UserId $userId
 } catch {
  foreach($i in $changed) { Copy-Item -LiteralPath ($paths[$i]+'.backup') -Destination $paths[$i] -Force }
  throw
 } finally {
  foreach($path in $paths) { if(Test-Path -LiteralPath ($path+'.new')) { Remove-Item -LiteralPath ($path+'.new') -Force } }
 }
}
try{
 if($Action -eq 'save'){ Save-Settings }
 else {
  $name=if($Component -eq 'sunshine'){'sunshine.exe'}else{'mslink.exe'}
  if($Component -eq 'all'){throw 'Choose a service.'}
  if($Action -in @('stop','restart')){Get-CimInstance Win32_Process -Filter "Name='$name'" | Where-Object {$_.ExecutablePath -and ([IO.Path]::GetFullPath($_.ExecutablePath)).StartsWith($Root+'\',[StringComparison]::OrdinalIgnoreCase)} | ForEach-Object {Stop-Process -Id $_.ProcessId -Force}}
  if($Action -in @('start','restart')){
   $existing=@(Get-CimInstance Win32_Process -Filter "Name='$name'" | Where-Object {$_.ExecutablePath -and ([IO.Path]::GetFullPath($_.ExecutablePath)).StartsWith($Root+'\',[StringComparison]::OrdinalIgnoreCase)})
   if($existing.Count -eq 0){$script=if($Component -eq 'sunshine'){'run-sunshine.ps1'}else{'run-tunnel.ps1'};& (Join-Path $Root ('_internal\'+$script)) -TargetDir $Root}
  }
 }
 Add-Content -LiteralPath $log -Value ((Get-Date -Format o)+' '+$Action+' '+$Component+' completed') -Encoding UTF8
 exit 0
}catch{Add-Content -LiteralPath $log -Value ((Get-Date -Format o)+' '+$Action+' failed: '+$_.Exception.Message) -Encoding UTF8;exit 1}
