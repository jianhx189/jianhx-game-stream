[Code]
var
  TransferCaption: TNewStaticText;
  TransferTrack, TransferFill: TPanel;

<event('InitializeWizard')>
procedure CreateTransferDisplay;
begin
  TransferCaption := TNewStaticText.Create(WizardForm);
  with TransferCaption do begin
    Parent := WizardForm.InstallingPage;
    Left := WizardForm.ProgressGauge.Left;
    Top := WizardForm.ProgressGauge.Top + WizardForm.ProgressGauge.Height + ScaleY(17);
    Font.Style := [fsBold];
    Font.Color := $00554B20;
    Caption := 'MiniStream';
  end;
  TransferTrack := TPanel.Create(WizardForm);
  with TransferTrack do begin
    Parent := WizardForm.InstallingPage;
    Left := TransferCaption.Left;
    Top := TransferCaption.Top + TransferCaption.Height + ScaleY(7);
    Width := WizardForm.ProgressGauge.Width;
    Height := ScaleY(4);
    BevelOuter := bvNone;
    Color := $00E6E1DA;
  end;
  TransferFill := TPanel.Create(WizardForm);
  with TransferFill do begin
    Parent := TransferTrack;
    SetBounds(0, 0, 0, TransferTrack.Height);
    BevelOuter := bvNone;
    Color := $008C7040;
  end;
end;

procedure CurInstallProgressChanged(CurProgress, MaxProgress: Integer);
var Percent: Integer;
begin
  if MaxProgress <= 0 then exit;
  Percent := Round(100.0 * CurProgress / MaxProgress);
  TransferFill.Width := Round(TransferTrack.Width * (CurProgress / MaxProgress));
  TransferCaption.Caption := 'MiniStream   ·   正在部署文件   ' + IntToStr(Percent) + '%';
end;
