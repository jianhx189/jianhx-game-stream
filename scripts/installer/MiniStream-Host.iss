; =============================================================================
; MiniStream-Host.iss —— MiniStream 被控端 Inno Setup 安装包
; 用 ISCC.exe 编译（本文件必须保存为 UTF-8 带 BOM）
; =============================================================================

#define HostSourceDir "D:\远程项目\构建产物\host"

[Setup]
AppId={{7F2A1C4E-9B3D-4E6A-A1F2-5A1B2C3D4E01}
AppName=MiniStream 被控端
AppVersion=2.3.1
AppPublisher=jianhx2026
AppCopyright=Powered by jianhx2026
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
DisableWelcomePage=no
DefaultDirName={code:GetHostInstallDir}
DisableDirPage=no
PrivilegesRequired=admin
OutputDir=D:\远程项目\构建产物\setup
OutputBaseFilename=MiniStream-Host-Setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
UninstallDisplayName=MiniStream 被控端
SetupLogging=yes

[Files]
; 整个被控端先解到 {tmp}\mspkg，再由 install.ps1 复制到安装目录
Source: "{#HostSourceDir}\*"; DestDir: "{tmp}\mspkg"; Flags: ignoreversion recursesubdirs createallsubdirs; Excludes: "*.log,*.dmp,*.pdb,*.obj,mslink-status.json,mslink-status.jsonl,mslink-status.jsonl.previous,sunshine\config\*,mslink-host.ini,_internal\*.log"

[Icons]
Name: "{commondesktop}\MiniStream 被控端"; Filename: "{app}\console\MiniStreamHost.exe"; Parameters: "--root ""{app}"""; WorkingDir: "{app}\console"
Name: "{group}\MiniStream 被控端"; Filename: "{app}\console\MiniStreamHost.exe"; Parameters: "--root ""{app}"""; WorkingDir: "{app}\console"
[Run]
Filename: "{app}\console\MiniStreamHost.exe"; Parameters: "--root ""{app}"""; WorkingDir: "{app}\console"; Description: "打开 MiniStream 被控端控制台"; Flags: nowait postinstall skipifsilent runasoriginaluser; Check: CanOpenConsole

[UninstallDelete]
Type: filesandordirs; Name: "{app}"

[Code]
var
  PortPage: TInputQueryWizardPage;
  AdvPage: TWizardPage;
  StartCheck: TNewCheckBox;
  InstallOk: Boolean;

function CanOpenConsole: Boolean;
begin
  Result := InstallOk;
end;

function GetHostInstallDir(Param: String): String;
var Existing: String;
begin
  Result := ExpandConstant('{autopf}\MiniStream-Host');
  if RegQueryStringValue(HKLM64, 'Software\Microsoft\Windows\CurrentVersion\Uninstall\{7F2A1C4E-9B3D-4E6A-A1F2-5A1B2C3D4E01}_is1', 'Inno Setup: App Path', Existing) or
     RegQueryStringValue(HKLM32, 'Software\Microsoft\Windows\CurrentVersion\Uninstall\{7F2A1C4E-9B3D-4E6A-A1F2-5A1B2C3D4E01}_is1', 'Inno Setup: App Path', Existing) then begin
    if FileExists(Existing + '\mslink-host.ini') then Result := Existing;
  end else if FileExists(ExpandConstant('{pf32}\MiniStream-Host\mslink-host.ini')) then
    Result := ExpandConstant('{pf32}\MiniStream-Host');
end;

function ReadHostSetting(Path, Key, Fallback: String): String;
var Lines: TArrayOfString; I, P: Integer; Line: String;
begin
  Result := Fallback;
  if not LoadStringsFromFile(Path, Lines) then exit;
  for I := 0 to GetArrayLength(Lines) - 1 do begin
    Line := Trim(Lines[I]);
    P := Pos('=', Line);
    if (P > 0) and (CompareText(Trim(Copy(Line, 1, P - 1)), Key) = 0) then begin
      Result := Trim(Copy(Line, P + 1, Length(Line))); exit;
    end;
  end;
end;

function RandPwd(Len: Integer): String;
var
  Chars: String;
  I: Integer;
begin
  Chars := 'ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnpqrstuvwxyz23456789';
  Result := '';
  for I := 1 to Len do
    Result := Result + Chars[Random(Length(Chars)) + 1];
end;

procedure InitializeWizard;
begin
  InstallOk := False;
  PortPage := CreateInputQueryPage(wpSelectDir,
    '隧道端口与口令', 'MiniStream 被控端网络配置',
    '设置本机监听端口和访问口令。装完后可在被控端「连接设置」复制或更换口令。');
  PortPage.Add('隧道 TCP 端口：', False);
  PortPage.Add('隧道 UDP 端口：', False);
  PortPage.Add('隧道口令：', True);
  PortPage.Values[0] := '14738';
  PortPage.Values[1] := '14739';
  PortPage.Values[2] := RandPwd(20);

  AdvPage := CreateCustomPage(PortPage.ID,
    '后台服务', '网络模板可在安装后的控制台选择');
  StartCheck := TNewCheckBox.Create(AdvPage);
  StartCheck.Parent := AdvPage.Surface;
  StartCheck.Left := 0;
  StartCheck.Top := 0;
  StartCheck.Width := AdvPage.SurfaceWidth;
  StartCheck.Caption := '安装完成后立即启动 Sunshine 和隧道';
  StartCheck.Checked := True;
end;

function NextButtonClick(CurPageID: Integer): Boolean;
var
  T, U: Integer;
  Pw: String;
  I: Integer;
  Ok: Boolean;
begin
  Result := True;
  if CurPageID = wpSelectDir then begin
    if FileExists(ExpandConstant('{app}\mslink-host.ini')) then begin
      PortPage.Values[0] := ReadHostSetting(ExpandConstant('{app}\mslink-host.ini'), 'tcp_port', '14738');
      PortPage.Values[1] := ReadHostSetting(ExpandConstant('{app}\mslink-host.ini'), 'udp_port', '14739');
      PortPage.Values[2] := ReadHostSetting(ExpandConstant('{app}\mslink-host.ini'), 'password', '');
      PortPage.Edits[0].Enabled := False;
      PortPage.Edits[1].Enabled := False;
      PortPage.Edits[2].Enabled := False;
      PortPage.SubCaptionLabel.Caption := '检测到已有配置：升级保留原端口和口令。之后可在控制台调整。';
    end;
  end;
  if CurPageID = PortPage.ID then begin
    T := StrToIntDef(Trim(PortPage.Values[0]), -1);
    U := StrToIntDef(Trim(PortPage.Values[1]), -1);
    if (T < 1) or (T > 65535) or (U < 1) or (U > 65535) then begin
      MsgBox('端口必须是 1-65535 的数字。', mbError, MB_OK);
      Result := False; exit;
    end;
    if T = U then begin
      MsgBox('TCP 端口和 UDP 端口不能相同。', mbError, MB_OK);
      Result := False; exit;
    end;
    Pw := Trim(PortPage.Values[2]);
    if FileExists(ExpandConstant('{app}\mslink-host.ini')) then
      Ok := (Length(Pw) >= 1) and (Length(Pw) <= 64)
    else
      Ok := (Length(Pw) >= 12) and (Length(Pw) <= 64);
    if Ok then
      for I := 1 to Length(Pw) do
        if Pos(Pw[I], 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_.@-') = 0 then begin
          Ok := False; break;
        end;
    if not Ok then begin
      MsgBox('新安装口令须为 12-64 个字母、数字或 _ . @ -；升级保留的旧口令可暂时不变，建议尽快更换。', mbError, MB_OK);
      Result := False; exit;
    end;
    PortPage.Values[0] := IntToStr(T);
    PortPage.Values[1] := IntToStr(U);
    PortPage.Values[2] := Pw;
  end;
end;

function PsQuote(S: String): String;
var
  I: Integer;
begin
  Result := '';
  for I := 1 to Length(S) do begin
    if S[I] = '''' then Result := Result + ''''''
    else Result := Result + S[I];
  end;
  Result := '''' + Result + '''';
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  Ini, WrapperPath, Ps1, PsExe, Args: String;
  OutPath, CmdExe: String;
  OutText: AnsiString;
  ResultCode: Integer;
  Launched: Boolean;
  PkgDir: String;
begin
  if CurStep <> ssPostInstall then exit;

  PkgDir := ExpandConstant('{tmp}\mspkg');
  { ---- 1. 按向导填写内容写 mslink-host.ini（覆盖包内默认配置） ---- }
  Ini := '; MiniStream host tunnel config (written by installer)' + #13#10 +
         'tcp_port = ' + PortPage.Values[0] + #13#10 +
         'udp_port = ' + PortPage.Values[1] + #13#10 +
         'password = ' + PortPage.Values[2] + #13#10 +
         'target = 127.0.0.1' + #13#10 +
         'bind = 0.0.0.0' + #13#10;
  if not SaveStringToFile(PkgDir + '\mslink-host.ini', Ini, False) then begin
    Log('写入隧道配置失败：' + PkgDir + '\mslink-host.ini');
    if not WizardSilent then
      MsgBox('写入隧道配置失败：' + PkgDir + '\mslink-host.ini', mbError, MB_OK);
    exit;
  end;

  { ---- 2. Sunshine 网页口令由 install.ps1 生成，避免安装器命令行传递 ---- }
  { ---- 3. 写临时 .ps1 包装脚本（UTF-8 BOM），再执行，避免命令行拼中文/长参数 ---- }
  { 用 & 调用符执行脚本；包装脚本全 ASCII，不写 BOM }
  Ps1 := '& ' + PsQuote(PkgDir + '\_internal\install.ps1') +
    ' -TargetDir ' + PsQuote(ExpandConstant('{app}')) +
    ' -WebUser admin' +
    ' -NoPause' +
    ' -LogPath ' + PsQuote(ExpandConstant('{app}\install.log'));
  if not StartCheck.Checked then
    Ps1 := Ps1 + ' -SkipSunshineLaunch -SkipTunnelStart';
  Ps1 := Ps1 + #13#10 + 'exit $LASTEXITCODE' + #13#10;

  WrapperPath := ExpandConstant('{tmp}\ministream-host-install.ps1');
  { 包装脚本只含 ASCII（路径/参数无中文），不写 BOM，避免 PowerShell 5.1 解析异常 }
  if not SaveStringToFile(WrapperPath, Ps1, False) then begin
    Log('无法创建安装脚本：' + WrapperPath);
    if not WizardSilent then
      MsgBox('无法创建安装脚本：' + WrapperPath, mbError, MB_OK);
    exit;
  end;

  PsExe := ExpandConstant('{win}\System32\WindowsPowerShell\v1.0\powershell.exe');
  { 输出重定向到文件，失败时把内容读进 Inno 日志 }
  OutPath := ExpandConstant('{tmp}\ministream-host-install-out.log');
  CmdExe := ExpandConstant('{cmd}');
  Args := '/c ""' + PsExe + '" -NoProfile -ExecutionPolicy Bypass -File "' +
    WrapperPath + '" > "' + OutPath + '" 2>&1"';
  Log('执行安装脚本：' + PsExe + ' -File ' + WrapperPath);
  ResultCode := 1;
  Launched := ShellExec('', CmdExe, Args, '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  if not Launched then begin
    Log('无法启动 PowerShell 执行安装脚本。');
    if not WizardSilent then
      MsgBox('无法启动 PowerShell 执行安装脚本。', mbError, MB_OK);
    exit;
  end;
  DeleteFile(PkgDir + '\_internal\web-credential.txt');
  DeleteFile(WrapperPath);
  Log('install.ps1 退出码：' + IntToStr(ResultCode));
  if LoadStringFromFile(OutPath, OutText) then
    Log('install.ps1 输出：' + OutText);
  if ResultCode <> 0 then begin
    Log('安装脚本执行失败（退出码 ' + IntToStr(ResultCode) + '）。详细日志：' +
      ExpandConstant('{app}\install.log'));
    if not WizardSilent then
      MsgBox('安装脚本执行失败（退出码 ' + IntToStr(ResultCode) + '）。' + #13#10 +
        '详细日志：' + ExpandConstant('{app}\install.log'), mbError, MB_OK);
    exit;
  end;
  InstallOk := True;
  { Retire older generated guides and backups that contained superseded credentials. }
  DeleteFile(ExpandConstant('{app}\配置指南.lnk'));
  DeleteFile(ExpandConstant('{app}\配置指南.html'));
  DeleteFile(ExpandConstant('{app}\mslink-host.ini.backup'));
end;

procedure CurPageChanged(CurPageID: Integer);
begin
  if CurPageID <> wpFinished then exit;
  if InstallOk then
    WizardForm.FinishedLabel.Caption :=
      'MiniStream 被控端安装完成。' + #13#10 + #13#10 +
      '下一步：打开被控端控制台 → 连接设置 → 选择局域网或 FRP → 复制连接信息。' + #13#10 +
      '再到另一台电脑打开主控端，填写信息并完成配对。' + #13#10 + #13#10 +
      '服务状态和 Windows 防火墙可以在「服务与诊断」中一键自检。'
  else
    WizardForm.FinishedLabel.Caption := '安装未完全成功，请查看安装目录下的 install.log。';
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  Ps1, WrapperPath, PsExe, Args, Root: String;
  ResultCode: Integer;
begin
  if CurUninstallStep <> usUninstall then exit;

  { 原版 uninstall.ps1 的 Assert-SafeTarget 要求目录名必须是 MiniStream，
    无法用于 MiniStream-Host，这里写临时 ps1 做同样的清理：
    删两条计划任务、删两条防火墙规则、按路径结束安装目录下的进程。 }
  Root := ExpandConstant('{app}');
  Ps1 :=
    '$root = ' + PsQuote(Root) + #13#10 +
    '& cmd /c ''schtasks /end /tn "MiniStreamTunnel" >nul 2>&1''' + #13#10 +
    '& cmd /c ''schtasks /delete /tn "MiniStreamTunnel" /f >nul 2>&1''' + #13#10 +
    '& cmd /c ''schtasks /end /tn "MiniStreamSunshine" >nul 2>&1''' + #13#10 +
    '& cmd /c ''schtasks /delete /tn "MiniStreamSunshine" /f >nul 2>&1''' + #13#10 +
    '& cmd /c ''netsh advfirewall firewall delete rule name="MiniStream Tunnel TCP" >nul 2>&1''' + #13#10 +
    '& cmd /c ''netsh advfirewall firewall delete rule name="MiniStream Tunnel UDP" >nul 2>&1''' + #13#10 +
    '$prefix = $root.TrimEnd(''\'') + ''\''' + #13#10 +
    'Get-CimInstance Win32_Process -ErrorAction SilentlyContinue | Where-Object {' + #13#10 +
    '  $_.ExecutablePath -and $_.ExecutablePath.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)' + #13#10 +
    '} | ForEach-Object { try { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue } catch {} }' + #13#10 +
    'Start-Sleep -Milliseconds 500' + #13#10 +
    'exit 0' + #13#10;

  WrapperPath := ExpandConstant('{tmp}\ministream-host-uninstall.ps1');
  if SaveStringToFile(WrapperPath, Ps1, False) then begin
    PsExe := ExpandConstant('{win}\System32\WindowsPowerShell\v1.0\powershell.exe');
    Args := '-NoProfile -ExecutionPolicy Bypass -File "' + WrapperPath + '"';
    ShellExec('runas', PsExe, Args, '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    DeleteFile(WrapperPath);
  end;
end;

#include "vc-runtime.iss"
#include "install-progress.iss"
