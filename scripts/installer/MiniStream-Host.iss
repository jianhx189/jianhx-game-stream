; =============================================================================
; MiniStream-Host.iss —— MiniStream 被控端 Inno Setup 安装包
; 用 ISCC.exe 编译（本文件必须保存为 UTF-8 带 BOM）
; =============================================================================

#define HostSourceDir "D:\远程项目\构建产物\host"

[Setup]
AppId={{7F2A1C4E-9B3D-4E6A-A1F2-5A1B2C3D4E01}
AppName=MiniStream 被控端
AppVersion=2.2.0
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
Name: "{app}\配置指南"; Filename: "{app}\配置指南.html"; WorkingDir: "{app}"; Comment: "MiniStream 被控端 配置指南（局域网 / FRP）"

[UninstallDelete]
Type: filesandordirs; Name: "{app}"

[Code]
var
  PortPage: TInputQueryWizardPage;
  AdvPage: TWizardPage;
  GuidePage: TOutputMsgMemoWizardPage;
  LanIp: String;
  FrpCheck: TNewCheckBox;
  StartCheck: TNewCheckBox;
  FrpServerEdit, FrpPortEdit, SunHttpEdit, SunHttpsEdit: TNewEdit;
  WebPassword: String;
  InstallOk: Boolean;
  ExistingWebCredentials: Boolean;

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

function ReadInfoValue(Path, Prefix, Fallback: String): String;
var Lines: TArrayOfString; I, P: Integer; Line: String;
begin
  Result := Fallback;
  if not LoadStringsFromFile(Path, Lines) then exit;
  for I := 0 to GetArrayLength(Lines) - 1 do begin
    Line := Trim(Lines[I]);
    P := Pos(Prefix, Line);
    if P = 1 then begin
      Result := Trim(Copy(Line, Length(Prefix) + 1, Length(Line))); exit;
    end;
  end;
end;

function IsDocker172(Ip: String): Boolean;
var
  P1, P2, Second: Integer;
begin
  { 172.16.0.0 - 172.31.255.255 判为虚拟网段（Docker/WSL/Hyper-V 默认） }
  Result := False;
  if Copy(Ip, 1, 4) <> '172.' then exit;
  P1 := 5;
  P2 := P1;
  while (P2 <= Length(Ip)) and (Ip[P2] <> '.') do Inc(P2);
  Second := StrToIntDef(Copy(Ip, P1, P2 - P1), -1);
  if (Second >= 16) and (Second <= 31) then Result := True;
end;

function GetLanIp(): String;
var
  OutPath: String;
  S: AnsiString;
  M, I, J: Integer;
  Line, Ip, Best, Fallback: String;
begin
  Result := '';
  OutPath := ExpandConstant('{tmp}\ministream-lanip.txt');
  if Exec(ExpandConstant('{cmd}'),
    '/c ipconfig | findstr /R /C:"IPv4" > "' + OutPath + '"',
    '', SW_HIDE, ewWaitUntilTerminated, I) and LoadStringFromFile(OutPath, S) then begin
    { 遍历所有 IPv4：优先 192.168.x.x，其次 10.x.x.x；
      跳过 172.16-31.x（多为 Docker/WSL 虚拟网段）和 169.254.x（APIPA）}
    Best := '';
    Fallback := '';
    repeat
      J := Pos(#13#10, String(S));
      if J > 0 then begin
        Line := Copy(String(S), 1, J - 1);
        Delete(S, 1, J + 1);
      end else begin
        Line := String(S);
        S := '';
      end;
      M := 0;
      for I := 1 to Length(Line) do
        if Line[I] = ':' then M := I;
      if M = 0 then continue;
      Ip := Copy(Line, M + 1, Length(Line));
      while (Length(Ip) > 0) and (Ip[1] = ' ') do Delete(Ip, 1, 1);
      while (Length(Ip) > 0) and (Ip[Length(Ip)] < #33) do Delete(Ip, Length(Ip), 1);
      if Ip = '' then continue;
      if (Copy(Ip, 1, 9) = '192.168.') then begin Best := Ip; break; end;
      if (Copy(Ip, 1, 3) = '10.') and (Best = '') then Best := Ip
      else if (Copy(Ip, 1, 8) <> '169.254.') and (not IsDocker172(Ip))
              and (Fallback = '') then Fallback := Ip;
    until Length(S) = 0;
    if Best <> '' then Result := Best else Result := Fallback;
  end;
  if Result = '' then Result := '（未检测到，请自行运行 ipconfig 查看）';
end;

function H(S: String): String;
var
  I: Integer;
begin
  Result := '';
  for I := 1 to Length(S) do
    if Ord(S[I]) > 127 then Result := Result + '&#' + IntToStr(Ord(S[I])) + ';'
    else Result := Result + S[I];
end;

function BuildGuideHtml(LanIp, TcpPort, UdpPort, TunnelPwd, WebPwd, AppDir: String): String;
begin
  Result :=
'<!DOCTYPE html><html lang="zh-CN"><head><meta charset="utf-8">' +
'<title>' + H('MiniStream 被控端 · 配置指南') + '</title><style>' +
'body{font-family:"Microsoft YaHei",sans-serif;background:#141821;color:#dfe6f2;margin:0;padding:32px;line-height:1.75}' +
'h1{color:#35d39a;font-size:22px}h2{color:#7cc4ff;font-size:17px;margin-top:28px}' +
'code{background:#0d1117;border:1px solid #2a3341;border-radius:5px;padding:1px 6px;color:#ffd479;font-family:Consolas,monospace}' +
'pre{background:#0d1117;border:1px solid #2a3341;border-radius:8px;padding:12px;overflow:auto;color:#c8e1ff;font-family:Consolas,monospace;font-size:12.5px}' +
'.card{background:#1a2130;border:1px solid #2a3341;border-radius:10px;padding:16px 20px;margin:14px 0}' +
'.k{color:#ffd479;font-weight:bold}.tip{color:#9aa7ba;font-size:13px}' +
'</style></head><body>' +
'<h1>' + H('MiniStream 被控端 · 配置指南') + '</h1>' +
'<div class="card"><b>' + H('本次安装信息（主控端首次连接要填，建议拍照/截图保存）：') + '</b><br>' +
H('本机局域网 IP：') + '<span class="k">' + LanIp + '</span><br>' +
H('隧道端口：TCP ') + '<span class="k">' + TcpPort + '</span> / UDP <span class="k">' + UdpPort + '</span><br>' +
H('隧道口令：') + '<span class="k">' + TunnelPwd + '</span><br>' +
H('Sunshine 管理密码：') + '<span class="k">' + WebPwd + '</span>' + H('（用户名 admin，管理页 https://localhost:47990）') + '</div>' +
'<h2>' + H('场景 A：局域网直连（两台机器接同一个路由器，最简单）') + '</h2>' +
'<div class="card">' + H('被控端装完即可，') + '<b>' + H('不需要 FRP') + '</b>' + H('。<br>主控端首次运行向导选「局域网直连」，地址填本机局域网 IP ') + '<code>' + LanIp + '</code>' + H('，端口用上面的 TCP/UDP 端口，口令填隧道口令。') + '</div>' +
'<h2>' + H('场景 B：FRP 内网穿透（两台机器不在同一个网络，我有 FRP 服务器）') + '</h2>' +
'<div class="card">' + H('1. 在你的 FRP 服务端配置两条转发：公网 TCP 端口 → 本机 ') + '<code>' + TcpPort + '</code>' + H('，公网 UDP 端口 → 本机 ') + '<code>' + UdpPort + '</code>' + H('。<br>') +
H('2. 如果 FRP 转发到的本地端口不是 ') + '<code>' + TcpPort + '</code> / <code>' + UdpPort + '</code>' + H('，用管理员身份编辑 ') + '<code>' + AppDir + '\mslink-host.ini</code>' + H('，把 ') + '<code>tcp_port</code> / <code>udp_port</code>' + H(' 改成转发到的本地端口，然后重新运行安装目录下的 ') + '<code>' + H('安装') + '.bat</code>' + H('。<br>') +
H('3. 主控端向导选「FRP 内网穿透」，填 FRP 公网地址和公网端口。') + '</div>' +
'<h2>' + H('场景 C：不同网络，还没有 FRP 服务器') + '</h2>' +
'<div class="card">' + H('需要一台有公网 IP 的机器跑 FRP 服务端（frps），或使用现成的 FRP 服务。<br>') +
H('frps.ini 示例：') + '<pre>[common]' + #13#10 + 'bind_port = 7000</pre>' +
H('被控端 frpc.ini 示例：') + '<pre>[common]' + #13#10 +
'server_addr = ' + H('你的FRP服务器地址') + #13#10 +
'server_port = 7000' + #13#10 + #13#10 +
'[ministream-tcp]' + #13#10 + 'type = tcp' + #13#10 + 'local_ip = 127.0.0.1' + #13#10 +
'local_port = ' + TcpPort + #13#10 + 'remote_port = 24738' + #13#10 + #13#10 +
'[ministream-udp]' + #13#10 + 'type = udp' + #13#10 + 'local_ip = 127.0.0.1' + #13#10 +
'local_port = ' + UdpPort + #13#10 + 'remote_port = 24739</pre>' +
H('配好后主控端填 ') + '<code>' + H('你的FRP服务器地址') + '</code>' + H('，端口填 ') + '<code>24738 / 24739</code>' + H('。') + '</div>' +
'<p class="tip">' + H('本页随安装生成，位于 ') + AppDir + H('\配置指南.html，桌面/开始菜单也有入口。') + '</p>' +
'</body></html>';
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

procedure FrpCheckClick(Sender: TObject);
begin
  FrpServerEdit.Enabled := FrpCheck.Checked;
  FrpPortEdit.Enabled := FrpCheck.Checked;
  SunHttpEdit.Enabled := FrpCheck.Checked;
  SunHttpsEdit.Enabled := FrpCheck.Checked;
end;

procedure InitializeWizard;
var
  Lbl: TNewStaticText;
  Y: Integer;
begin
  InstallOk := False;
  WebPassword := '';
  LanIp := GetLanIp();

  { ---- 第 1 页：端口与口令 ---- }
  PortPage := CreateInputQueryPage(wpSelectDir,
    '隧道端口与口令', 'MiniStream 被控端网络配置',
    '请设置隧道监听端口和访问口令（主控端连接时要填一样的）。');
  PortPage.Add('隧道 TCP 端口：', False);
  PortPage.Add('隧道 UDP 端口：', False);
  PortPage.Add('隧道口令（请记下来）：', True);
  PortPage.Values[0] := '14738';
  PortPage.Values[1] := '14739';
  PortPage.Values[2] := RandPwd(12);

  { ---- 第 2 页：FRP 高级 + 是否立即启动 ---- }
  AdvPage := CreateCustomPage(PortPage.ID,
    '高级选项', 'FRP 内网穿透（可选）与启动选项');

  FrpCheck := TNewCheckBox.Create(AdvPage);
  FrpCheck.Parent := AdvPage.Surface;
  FrpCheck.Left := 0;
  FrpCheck.Top := 0;
  FrpCheck.Width := AdvPage.SurfaceWidth;
  FrpCheck.Caption := '使用 FRP 内网穿透（高级）';
  FrpCheck.Checked := False;
  FrpCheck.OnClick := @FrpCheckClick;

  Y := FrpCheck.Top + FrpCheck.Height + ScaleY(14);

  Lbl := TNewStaticText.Create(AdvPage);
  Lbl.Parent := AdvPage.Surface; Lbl.Left := ScaleX(16); Lbl.Top := Y + ScaleY(3);
  Lbl.Caption := 'FRP 服务器地址：';
  FrpServerEdit := TNewEdit.Create(AdvPage);
  FrpServerEdit.Parent := AdvPage.Surface;
  FrpServerEdit.Left := ScaleX(170); FrpServerEdit.Top := Y;
  FrpServerEdit.Width := AdvPage.SurfaceWidth - FrpServerEdit.Left;
  Y := Y + ScaleY(30);

  Lbl := TNewStaticText.Create(AdvPage);
  Lbl.Parent := AdvPage.Surface; Lbl.Left := ScaleX(16); Lbl.Top := Y + ScaleY(3);
  Lbl.Caption := 'FRP 端口：';
  FrpPortEdit := TNewEdit.Create(AdvPage);
  FrpPortEdit.Parent := AdvPage.Surface;
  FrpPortEdit.Left := ScaleX(170); FrpPortEdit.Top := Y;
  FrpPortEdit.Width := AdvPage.SurfaceWidth - FrpPortEdit.Left;
  FrpPortEdit.Text := '7000';
  Y := Y + ScaleY(30);

  Lbl := TNewStaticText.Create(AdvPage);
  Lbl.Parent := AdvPage.Surface; Lbl.Left := ScaleX(16); Lbl.Top := Y + ScaleY(3);
  Lbl.Caption := '本地 Sunshine HTTP 端口：';
  SunHttpEdit := TNewEdit.Create(AdvPage);
  SunHttpEdit.Parent := AdvPage.Surface;
  SunHttpEdit.Left := ScaleX(170); SunHttpEdit.Top := Y;
  SunHttpEdit.Width := AdvPage.SurfaceWidth - SunHttpEdit.Left;
  SunHttpEdit.Text := '47989';
  Y := Y + ScaleY(30);

  Lbl := TNewStaticText.Create(AdvPage);
  Lbl.Parent := AdvPage.Surface; Lbl.Left := ScaleX(16); Lbl.Top := Y + ScaleY(3);
  Lbl.Caption := '本地 Sunshine HTTPS 端口：';
  SunHttpsEdit := TNewEdit.Create(AdvPage);
  SunHttpsEdit.Parent := AdvPage.Surface;
  SunHttpsEdit.Left := ScaleX(170); SunHttpsEdit.Top := Y;
  SunHttpsEdit.Width := AdvPage.SurfaceWidth - SunHttpsEdit.Left;
  SunHttpsEdit.Text := '47984';
  Y := Y + ScaleY(40);

  StartCheck := TNewCheckBox.Create(AdvPage);
  StartCheck.Parent := AdvPage.Surface;
  StartCheck.Left := 0; StartCheck.Top := Y;
  StartCheck.Width := AdvPage.SurfaceWidth;
  StartCheck.Caption := '安装完成后立即启动 Sunshine 和隧道';
  StartCheck.Checked := True;

  FrpCheckClick(nil);

  { ---- 第 3 页（完成页之前）：配置指南 ---- }
  GuidePage := CreateOutputMsgMemoPage(wpInstalling,
    '配置指南', '装完之后怎么让两台机器连起来',
    '下面按场景选择一种连接方式。安装完成后也可以在安装目录打开 配置指南.html 随时查看。',
    '【先记下这几个（主控端要填）】' + #13#10 +
    '  · 隧道口令、TCP/UDP 端口：就是前两页你填的值' + #13#10 +
    '  · Sunshine 管理密码：安装完成后会显示，也会写进 安装信息.txt' + #13#10 +
    '  · 本机局域网 IP：安装完成后会显示' + #13#10 + #13#10 +
    '【场景 A】局域网直连（两台机器接同一个路由器）—— 最简单，推荐' + #13#10 +
    '  1. 被控端装完即可，不需要 FRP。' + #13#10 +
    '  2. 主控端首次运行选「局域网直连」，地址填本机局域网 IP（如 192.168.1.x），' + #13#10 +
    '     端口/口令填上面记的。' + #13#10 + #13#10 +
    '【场景 B】FRP 内网穿透（不同网络，你有自己的 FRP 服务器）' + #13#10 +
    '  1. 在 FRP 服务端加两条转发：公网 TCP → 本机 14738，公网 UDP → 本机 14739。' + #13#10 +
    '  2. 若 FRP 转发到的本地端口不是 14738/14739：用管理员身份编辑' + #13#10 +
    '     C:\Program Files\MiniStream-Host\mslink-host.ini，把 tcp_port / udp_port' + #13#10 +
    '     改成 FRP 转发到的本地端口，再运行一次安装目录里的 安装.bat。' + #13#10 +
    '  3. 主控端选「FRP 内网穿透」，填 FRP 公网地址 + 公网 TCP/UDP 端口。' + #13#10 + #13#10 +
    '【场景 C】不同网络，还没有 FRP 服务器' + #13#10 +
    '  需要一台有公网 IP 的机器跑 frps，或用现成的 FRP 服务。' + #13#10 +
    '  frps.ini：  [common]  bind_port = 7000' + #13#10 +
    '  被控端 frpc.ini：' + #13#10 +
    '    [common]  server_addr = 你的FRP服务器  server_port = 7000' + #13#10 +
    '    [ministream-tcp] type=tcp local_ip=127.0.0.1 local_port=14738 remote_port=24738' + #13#10 +
    '    [ministream-udp] type=udp local_ip=127.0.0.1 local_port=14739 remote_port=24739' + #13#10 +
    '  主控端填：你的FRP服务器，端口 24738 / 24739。' + #13#10 + #13#10 +
    '  提示：安装完成页有「打开配置指南」按钮，会把以上内容（含 frpc 完整示例）' + #13#10 +
    '  以网页形式打开，方便照着抄。');
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
  if CurPageID = AdvPage.ID then begin
    if FrpCheck.Checked then begin
      if Trim(FrpServerEdit.Text) = '' then begin
        MsgBox('勾选 FRP 后请填写 FRP 服务器地址。', mbError, MB_OK);
        Result := False; exit;
      end;
      T := StrToIntDef(Trim(FrpPortEdit.Text), -1);
      if (T < 1) or (T > 65535) then begin
        MsgBox('FRP 端口必须是 1-65535 的数字。', mbError, MB_OK);
        Result := False; exit;
      end;
      T := StrToIntDef(Trim(SunHttpEdit.Text), -1);
      if (T < 1) or (T > 65535) then begin
        MsgBox('本地 Sunshine HTTP 端口必须是 1-65535 的数字。', mbError, MB_OK);
        Result := False; exit;
      end;
      T := StrToIntDef(Trim(SunHttpsEdit.Text), -1);
      if (T < 1) or (T > 65535) then begin
        MsgBox('本地 Sunshine HTTPS 端口必须是 1-65535 的数字。', mbError, MB_OK);
        Result := False; exit;
      end;
    end;
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
  ExistingWebCredentials := DirExists(ExpandConstant('{app}\sunshine\config\credentials')) or
    FileExists(ExpandConstant('{app}\sunshine\config\credentials'));

  { ---- 1. 按向导填写内容写 mslink-host.ini（覆盖包内默认配置） ---- }
  Ini := '; MiniStream host tunnel config (written by installer)' + #13#10 +
         'tcp_port = ' + PortPage.Values[0] + #13#10 +
         'udp_port = ' + PortPage.Values[1] + #13#10 +
         'password = ' + PortPage.Values[2] + #13#10 +
         'target = 127.0.0.1' + #13#10 +
         'bind = 0.0.0.0' + #13#10;
  if FrpCheck.Checked then begin
    Ini := Ini +
      'frp_server = ' + Trim(FrpServerEdit.Text) + #13#10 +
      'frp_port = ' + Trim(FrpPortEdit.Text) + #13#10 +
      'sunshine_http_port = ' + Trim(SunHttpEdit.Text) + #13#10 +
      'sunshine_https_port = ' + Trim(SunHttpsEdit.Text) + #13#10 +
      'line_main = TCP ' + Trim(FrpServerEdit.Text) + ':' + Trim(FrpPortEdit.Text) +
        ' / UDP ' + Trim(FrpServerEdit.Text) + ':' + Trim(FrpPortEdit.Text) + #13#10 +
      'line_backup = ' + #13#10;
  end else begin
    Ini := Ini +
      'line_main = ' + #13#10 +
      'line_backup = ' + #13#10;
  end;
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
  if FileExists(ExpandConstant('{app}\mslink-host.ini')) then begin
    PortPage.Values[0] := ReadHostSetting(ExpandConstant('{app}\mslink-host.ini'), 'tcp_port', PortPage.Values[0]);
    PortPage.Values[1] := ReadHostSetting(ExpandConstant('{app}\mslink-host.ini'), 'udp_port', PortPage.Values[1]);
    PortPage.Values[2] := ReadHostSetting(ExpandConstant('{app}\mslink-host.ini'), 'password', PortPage.Values[2]);
  end;
  if ExistingWebCredentials then
    WebPassword := '原有管理密码保持不变，请使用原安装信息'
  else
    WebPassword := ReadInfoValue(ExpandConstant('{app}\安装信息.txt'), 'Sunshine 网页口令：', '见安装信息.txt');

  { ---- 4. 生成 配置指南.html（纯 ASCII + 字符实体，任何系统区域设置下都显示正确） ---- }
  SaveStringToFile(ExpandConstant('{app}\配置指南.html'),
    BuildGuideHtml(LanIp, PortPage.Values[0], PortPage.Values[1],
                   PortPage.Values[2], WebPassword, ExpandConstant('{app}')), False);
  Ps1 := '. ' + PsQuote(ExpandConstant('{app}\_internal\tunnel-common.ps1')) + #13#10 +
    '$user = [Security.Principal.WindowsIdentity]::GetCurrent().Name' + #13#10 +
    'Protect-MiniStreamFile -Path ' + PsQuote(ExpandConstant('{app}\配置指南.html')) + ' -UserId $user' + #13#10;
  SaveStringToFile(WrapperPath, Ps1, False);
  Args := '-NoProfile -ExecutionPolicy Bypass -File "' + WrapperPath + '"';
  if not ShellExec('', PsExe, Args, '', SW_HIDE, ewWaitUntilTerminated, ResultCode) or (ResultCode <> 0) then
    Log('配置指南的访问权限未能收紧，请手工检查文件 ACL。');
  DeleteFile(WrapperPath);
end;

procedure OpenGuide(Sender: TObject);
var
  Err: Integer;
begin
  ShellExec('open', ExpandConstant('{app}\配置指南.html'), '', '', SW_SHOW, ewNoWait, Err);
end;

procedure CurPageChanged(CurPageID: Integer);
var
  Msg: String;
  GuideBtn: TNewButton;
begin
  if CurPageID = wpFinished then begin
    WizardForm.FinishedLabel.Height := ScaleY(240);
    if InstallOk then begin
      Msg := 'MiniStream 被控端安装完成。' + #13#10 + #13#10 +
        '请记下（主控端首次连接需要）：' + #13#10 +
        '  本机局域网 IP = ' + LanIp + #13#10 +
        '  Sunshine 管理密码 = ' + WebPassword + #13#10 +
        '  隧道口令 = ' + PortPage.Values[2] + #13#10 +
        '  隧道端口：TCP ' + PortPage.Values[0] + ' / UDP ' + PortPage.Values[1] + #13#10 + #13#10 +
        'Sunshine 管理页面：https://localhost:47990 （用户名 admin）' + #13#10 +
        '以上信息也保存在安装目录的 安装信息.txt 里。' + #13#10 +
        '两台机器怎么连，点下方「查看配置指南」。';
      GuideBtn := TNewButton.Create(WizardForm);
      GuideBtn.Parent := WizardForm.FinishedPage;
      GuideBtn.Left := WizardForm.FinishedLabel.Left;
      GuideBtn.Top := WizardForm.FinishedLabel.Top + WizardForm.FinishedLabel.Height + ScaleY(8);
      GuideBtn.Width := ScaleX(150);
      GuideBtn.Caption := '查看配置指南';
      GuideBtn.OnClick := @OpenGuide;
    end else begin
      Msg := '安装未完全成功，请查看安装目录下的 install.log。';
      WizardForm.FinishedLabel.Caption := Msg;
      exit;
    end;
    WizardForm.FinishedLabel.Caption := Msg;
  end;
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
