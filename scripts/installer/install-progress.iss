[Code]
var
  DeployCard, TransferTrack, TransferFill, SignalHead: TPanel;
  DeployBrand, DeployTitle, TransferCaption, DeployNote: TNewStaticText;

procedure DeployLabel(var Item: TNewStaticText; Left, Top, Size, Ink: Integer; Bold: Boolean; const Text: String);
begin
  Item := TNewStaticText.Create(WizardForm);
  Item.Parent := DeployCard;
  Item.Left := ScaleX(14) + ScaleX(Left);
  Item.Top := ScaleY(Top);
  Item.Font.Size := Size;
  Item.Font.Color := Ink;
  if Bold then Item.Font.Style := [fsBold];
  Item.Caption := Text;
end;

<event('InitializeWizard')>
procedure CreateTransferDisplay;
begin
  DeployCard := TPanel.Create(WizardForm);
  DeployCard.Parent := WizardForm.InstallingPage;
  DeployCard.BevelOuter := bvNone;
  DeployCard.Color := $00331E13;
  DeployCard.SetBounds(WizardForm.ProgressGauge.Left,
    WizardForm.ProgressGauge.Top + WizardForm.ProgressGauge.Height + ScaleY(13),
    WizardForm.ProgressGauge.Width, ScaleY(108));
  DeployLabel(DeployBrand, 0, 10, 9, $00F2C966, True, 'MINISTREAM  /  安装');
  DeployLabel(DeployTitle, 0, 29, 12, $00FFF5F0, True, '正在准备组件');
  DeployLabel(TransferCaption, 0, 32, 10, $00FFF5F0, True, '0%');
  TransferCaption.Left := DeployCard.Width - TransferCaption.Width - ScaleX(14);
  DeployLabel(DeployNote, 0, 79, 8, $00DDC2B1, False, 'Powered by jianhx2026');

  TransferTrack := TPanel.Create(WizardForm);
  TransferTrack.Parent := DeployCard;
  TransferTrack.BevelOuter := bvNone;
  TransferTrack.Color := $00513D29;
  TransferTrack.SetBounds(ScaleX(14), ScaleY(62), DeployCard.Width - ScaleX(28), ScaleY(6));

  TransferFill := TPanel.Create(WizardForm);
  TransferFill.Parent := TransferTrack;
  TransferFill.BevelOuter := bvNone;
  TransferFill.Color := $00F2AA51;
  TransferFill.SetBounds(0, 0, 0, TransferTrack.Height);

  SignalHead := TPanel.Create(WizardForm);
  SignalHead.Parent := TransferFill;
  SignalHead.BevelOuter := bvNone;
  SignalHead.Color := $00F5E6AB;
  SignalHead.SetBounds(0, 0, ScaleX(5), TransferTrack.Height);
end;

procedure CurInstallProgressChanged(CurProgress, MaxProgress: Integer);
var Percent, FillWidth: Integer;
begin
  if (MaxProgress <= 0) or (TransferFill = nil) then exit;
  Percent := Round(100.0 * CurProgress / MaxProgress);
  if Percent < 0 then Percent := 0;
  if Percent > 100 then Percent := 100;
  FillWidth := Round(TransferTrack.Width * CurProgress / MaxProgress);
  if FillWidth < 0 then FillWidth := 0;
  if FillWidth > TransferTrack.Width then FillWidth := TransferTrack.Width;
  TransferFill.Width := FillWidth;
  SignalHead.Visible := (FillWidth >= SignalHead.Width) and (Percent < 100);
  SignalHead.Left := FillWidth - SignalHead.Width;
  TransferCaption.Caption := IntToStr(Percent) + '%';
  TransferCaption.Left := DeployCard.Width - TransferCaption.Width - ScaleX(14);
  if Percent < 100 then DeployTitle.Caption := '正在部署文件'
  else DeployTitle.Caption := '文件部署完成';
end;
