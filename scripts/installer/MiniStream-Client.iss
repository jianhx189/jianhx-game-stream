; =============================================================================
; MiniStream-Client.iss —— MiniStream 主控端（Moonlight）Inno Setup 安装包
; 用 ISCC.exe 编译（本文件保存为 UTF-8 带 BOM）
; =============================================================================

#define ClientSourceDir "D:\远程项目\构建产物\client-preview"

[Setup]
AppId={{3C8E5A2B-1D4F-4B7C-9E2A-6D8F0A1B2C02}
AppName=MiniStream 主控端
AppVersion=2.3.2
VersionInfoVersion=2.3.2.0
AppPublisher=jianhx2026
AppCopyright=Powered by jianhx2026
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
DisableWelcomePage=no
DefaultDirName={autopf}\MiniStream
DisableDirPage=no
PrivilegesRequired=admin
OutputDir=D:\远程项目\构建产物\setup
OutputBaseFilename=MiniStream-Client-Setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
UninstallDisplayName=MiniStream 主控端
SetupLogging=yes

[Files]
Source: "{#ClientSourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs; Excludes: "Moonlight.exe,*.log,*.dmp,*.pdb,*.obj,mslink-status.json,mslink-status.jsonl,mslink-status.jsonl.previous,mslink.ini,_launcher\*.log"

[Registry]
Root: HKLM; Subkey: "Software\Classes\ministream"; ValueType: string; ValueData: "URL:MiniStream"; Flags: uninsdeletekey
Root: HKLM; Subkey: "Software\Classes\ministream"; ValueType: string; ValueName: "URL Protocol"; ValueData: ""
Root: HKLM; Subkey: "Software\Classes\ministream\shell\open\command"; ValueType: string; ValueData: """{app}\MiniStream.exe"" --open-link ""%1"""

[Icons]
; 客户端将配置保存到当前用户 AppLocalDataLocation，不向 Program Files 写入用户配置。
Name: "{commondesktop}\MiniStream 主控端"; Filename: "{app}\MiniStream.exe"; WorkingDir: "{app}"; Comment: "MiniStream 主控端（Moonlight 串流客户端）"

Name: "{group}\MiniStream 主控端"; Filename: "{app}\MiniStream.exe"; WorkingDir: "{app}"

[UninstallDelete]
Type: filesandordirs; Name: "{app}"


#include "vc-runtime.iss"
#include "install-progress.iss"

[Code]
function InitializeSetup: Boolean;
var Installed, VersionPart, RegPath: String;
    Suffix: Integer;
    Previous, Candidate: Int64;
begin
  Result := True;
  RegPath := 'Software\Microsoft\Windows\CurrentVersion\Uninstall\{3C8E5A2B-1D4F-4B7C-9E2A-6D8F0A1B2C02}_is1';
  if not RegQueryStringValue(HKLM64, RegPath, 'DisplayVersion', Installed) then
    if not RegQueryStringValue(HKLM32, RegPath, 'DisplayVersion', Installed) then exit;
  VersionPart := Installed;
  Suffix := Pos('-', VersionPart);
  if Suffix > 0 then VersionPart := Copy(VersionPart, 1, Suffix - 1);
  if not StrToVersion(VersionPart, Previous) or not StrToVersion('{#SetupSetting("AppVersion")}', Candidate) then exit;
  if (ComparePackedVersion(Previous, Candidate) < 0) or ((Previous = Candidate) and (Suffix > 0)) then
    Result := SuppressibleMsgBox('发现旧版 MiniStream 主控端 ' + Installed + '。' + #13#10 +
      '是否更新到 {#SetupSetting("AppVersion")}？已有连接配置将保留。', mbConfirmation, MB_YESNO, IDYES) = IDYES
  else if ComparePackedVersion(Previous, Candidate) > 0 then begin
    SuppressibleMsgBox('已安装更新的主控端 ' + Installed + '。请使用对应版本或更新的安装包。', mbInformation, MB_OK, IDOK);
    Result := False;
  end;
end;
