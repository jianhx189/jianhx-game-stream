[Files]
Source: "D:\远程项目\构建产物\prerequisites\vc_redist.x64.exe"; Flags: dontcopy

[Code]
function RuntimeInstalledInView(Root: Integer): Boolean;
var Installed, Major, Minor, Build: Cardinal;
begin
  Result := RegQueryDWordValue(Root, 'SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x64', 'Installed', Installed) and (Installed = 1) and
    RegQueryDWordValue(Root, 'SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x64', 'Major', Major) and
    RegQueryDWordValue(Root, 'SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x64', 'Minor', Minor) and
    RegQueryDWordValue(Root, 'SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x64', 'Bld', Build);
  if Result then Result := (Major > 14) or ((Major = 14) and ((Minor > 44) or ((Minor = 44) and (Build >= 35112))));
end;
function PrepareToInstall(var NeedsRestart: Boolean): String;
var Code: Integer;
begin
  Result := '';
  if RuntimeInstalledInView(HKLM64) or RuntimeInstalledInView(HKLM32) then exit;
  ExtractTemporaryFile('vc_redist.x64.exe');
  WizardForm.StatusLabel.Caption := '正在安装 Microsoft Visual C++ x64 运行库…';
  if not Exec(ExpandConstant('{tmp}\vc_redist.x64.exe'), '/install /quiet /norestart', '', SW_HIDE, ewWaitUntilTerminated, Code) then begin
    Result := '无法启动 Microsoft VC++ 运行库安装程序。'; exit;
  end;
  if (Code = 3010) or (Code = 1641) then NeedsRestart := True
  else if Code <> 0 then begin
    if not ((Code = 1638) and (RuntimeInstalledInView(HKLM64) or RuntimeInstalledInView(HKLM32))) then
      Result := 'VC++ 运行库安装失败，退出码 ' + IntToStr(Code) + '。请完成微软运行库安装后重试。';
  end;
end;