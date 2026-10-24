; =============================================================================
; MiniStream-Client.iss —— MiniStream 主控端（Moonlight）Inno Setup 安装包
; 用 ISCC.exe 编译（本文件保存为 UTF-8 带 BOM）
; =============================================================================

#define ClientSourceDir "D:\远程项目\构建产物\client-preview"

[Setup]
AppId={{3C8E5A2B-1D4F-4B7C-9E2A-6D8F0A1B2C02}
AppName=MiniStream 主控端
AppVersion=2.2.0
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

[Icons]
; 客户端将配置保存到当前用户 AppLocalDataLocation，不向 Program Files 写入用户配置。
Name: "{commondesktop}\MiniStream 主控端"; Filename: "{app}\MiniStream.exe"; WorkingDir: "{app}"; Comment: "MiniStream 主控端（Moonlight 串流客户端）"

Name: "{group}\MiniStream 主控端"; Filename: "{app}\MiniStream.exe"; WorkingDir: "{app}"

[UninstallDelete]
Type: filesandordirs; Name: "{app}"


#include "vc-runtime.iss"
