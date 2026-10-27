; Pacitum Browser installer - Inno Setup 6.6+ (CI pins 6.7.1). Design: design-1.5-extra/installer-spec.md
#define AppName "Pacitum Browser"
; CI passes /DAppVer=<version from build/pacitum-version.txt> (1.9.x or legacy 1.8.x)
#ifndef AppVer
#define AppVer "2.0.1"
#endif
#define AppExe "PacitumBrowser.exe"
[Setup]
; Same AppId as 1.0-1.4: Setup finds the existing install and upgrades it in place.
AppId={{FD3A16C9-4CF0-4B0B-BBEC-A5E3334D7CFE}
AppName={#AppName}
AppVersion={#AppVer}
AppVerName={#AppName} {#AppVer}
AppPublisher=Pacitum
DefaultDirName={autopf}\Pacitum Browser
DefaultGroupName=Pacitum Browser
UninstallDisplayIcon={app}\{#AppExe}
UninstallDisplayName={#AppName}
SetupIconFile=resources\pacitum.ico
OutputDir=installer
OutputBaseFilename=PacitumBrowser-Setup-{#AppVer}
Compression=lzma2/ultra64
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequiredOverridesAllowed=dialog
; --- look (installer-spec.md "Getting the Pacitum look") ---
; modern + dynamic light/dark + the built-in "zircon" light style (cyan/teal accents on buttons,
; progress bar and focus). No VCL .vsf style file: none exists for Pacitum yet, and the built-in
; styles need no extra files, so they behave the same locally and on CI.
WizardStyle=modern dynamic zircon hidebevels
WizardSizePercent=120
; Every DPI step (100-250 %): Setup picks the closest match for the monitor.
WizardImageFile=installer\assets\wizard-image-light-100.png,installer\assets\wizard-image-light-125.png,installer\assets\wizard-image-light-150.png,installer\assets\wizard-image-light-175.png,installer\assets\wizard-image-light-200.png,installer\assets\wizard-image-light-225.png,installer\assets\wizard-image-light-250.png
WizardImageFileDynamicDark=installer\assets\wizard-image-dark-100.png,installer\assets\wizard-image-dark-125.png,installer\assets\wizard-image-dark-150.png,installer\assets\wizard-image-dark-175.png,installer\assets\wizard-image-dark-200.png,installer\assets\wizard-image-dark-225.png,installer\assets\wizard-image-dark-250.png
WizardSmallImageFile=installer\assets\wizard-small-light-100.png,installer\assets\wizard-small-light-125.png,installer\assets\wizard-small-light-150.png,installer\assets\wizard-small-light-175.png,installer\assets\wizard-small-light-200.png,installer\assets\wizard-small-light-225.png,installer\assets\wizard-small-light-250.png
WizardSmallImageFileDynamicDark=installer\assets\wizard-small-dark-100.png,installer\assets\wizard-small-dark-125.png,installer\assets\wizard-small-dark-150.png,installer\assets\wizard-small-dark-175.png,installer\assets\wizard-small-dark-200.png,installer\assets\wizard-small-dark-225.png,installer\assets\wizard-small-dark-250.png
WizardImageBackColor=#FFFFFF
WizardImageBackColorDynamicDark=#202020
WizardImageStretch=no
; --- pages: Welcome > Location (+ disk space, tasks) > Installing > Finish ---
DisableWelcomePage=no
DisableDirPage=auto
DisableProgramGroupPage=yes
DisableReadyPage=yes
; --- in-place upgrade ---
UsePreviousAppDir=yes
UsePreviousGroup=yes
UsePreviousTasks=yes
UsePreviousPrivileges=yes
CloseApplications=yes
CloseApplicationsFilter=*.exe,*.dll
RestartApplications=yes
AppMutex=PacitumBrowserRunning
VersionInfoVersion={#AppVer}
VersionInfoProductName={#AppName}
LZMAUseSeparateProcess=yes
ChangesAssociations=yes

[Messages]
WelcomeLabel1=Welcome to the Pacitum Browser Setup Wizard
WelcomeLabel2=This will install [name/ver] on your computer.%n%nA calm, fast browser that stays out of your way. Your bookmarks, history and settings are kept when you upgrade.
FinishedHeadingLabel=Pacitum Browser is ready
FinishedLabel=Setup has finished installing [name] on your computer.
FinishedLabelNoIcons=Setup has finished installing [name] on your computer.
ClickFinish=Click Finish to close Setup.
SelectDirDesc=Where should Pacitum Browser be installed?

[InstallDelete]
; stale Qt files from older builds (user data lives in %LOCALAPPDATA%, not here)
Type: filesandordirs; Name: "{app}\translations"
Type: filesandordirs; Name: "{app}\resources\qtwebengine_devtools_resources.pak"

[Files]
Source: "dist\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\Pacitum Browser"; Filename: "{app}\{#AppExe}"
Name: "{group}\Uninstall Pacitum Browser"; Filename: "{uninstallexe}"
; desktop shortcut: checkbox on the location page (installer-spec "Tasks on the directory page")
Name: "{autodesktop}\Pacitum Browser"; Filename: "{app}\{#AppExe}"; Check: WantDesktopIcon

[Registry]
; Register as a web browser so it appears in Settings > Default apps (the welcome flow's
; "Set as default" opens ms-settings:defaultapps?registeredAppUser=Pacitum Browser).
Root: HKA; Subkey: "Software\Classes\PacitumHTML"; ValueType: string; ValueData: "Pacitum HTML Document"; Flags: uninsdeletekey
Root: HKA; Subkey: "Software\Classes\PacitumHTML\DefaultIcon"; ValueType: string; ValueData: "{app}\{#AppExe},0"
Root: HKA; Subkey: "Software\Classes\PacitumHTML\shell\open\command"; ValueType: string; ValueData: """{app}\{#AppExe}"" ""%1"""
Root: HKA; Subkey: "Software\Clients\StartMenuInternet\Pacitum Browser"; ValueType: string; ValueData: "Pacitum Browser"; Flags: uninsdeletekey
Root: HKA; Subkey: "Software\Clients\StartMenuInternet\Pacitum Browser\DefaultIcon"; ValueType: string; ValueData: "{app}\{#AppExe},0"
Root: HKA; Subkey: "Software\Clients\StartMenuInternet\Pacitum Browser\shell\open\command"; ValueType: string; ValueData: """{app}\{#AppExe}"""
Root: HKA; Subkey: "Software\Clients\StartMenuInternet\Pacitum Browser\Capabilities"; ValueType: string; ValueName: "ApplicationName"; ValueData: "Pacitum Browser"
Root: HKA; Subkey: "Software\Clients\StartMenuInternet\Pacitum Browser\Capabilities"; ValueType: string; ValueName: "ApplicationDescription"; ValueData: "A calm, fast browser that stays out of your way."
Root: HKA; Subkey: "Software\Clients\StartMenuInternet\Pacitum Browser\Capabilities"; ValueType: string; ValueName: "ApplicationIcon"; ValueData: "{app}\{#AppExe},0"
Root: HKA; Subkey: "Software\Clients\StartMenuInternet\Pacitum Browser\Capabilities\URLAssociations"; ValueType: string; ValueName: "http"; ValueData: "PacitumHTML"
Root: HKA; Subkey: "Software\Clients\StartMenuInternet\Pacitum Browser\Capabilities\URLAssociations"; ValueType: string; ValueName: "https"; ValueData: "PacitumHTML"
Root: HKA; Subkey: "Software\Clients\StartMenuInternet\Pacitum Browser\Capabilities\FileAssociations"; ValueType: string; ValueName: ".html"; ValueData: "PacitumHTML"
Root: HKA; Subkey: "Software\Clients\StartMenuInternet\Pacitum Browser\Capabilities\FileAssociations"; ValueType: string; ValueName: ".htm"; ValueData: "PacitumHTML"
Root: HKA; Subkey: "Software\RegisteredApplications"; ValueType: string; ValueName: "Pacitum Browser"; ValueData: "Software\Clients\StartMenuInternet\Pacitum Browser\Capabilities"; Flags: uninsdeletevalue

[Run]
Filename: "{app}\{#AppExe}"; Description: "Launch Pacitum Browser"; Flags: nowait postinstall skipifsilent
; Built-in updater (1.5.2): the browser runs Setup with /VERYSILENT ... /RELAUNCH=1 and has already
; exited; start the new version when the silent upgrade is done.
Filename: "{app}\{#AppExe}"; Flags: nowait runasoriginaluser; Check: RelaunchRequested

[Code]
var
  DiskLabel: TNewStaticText;
  DiskBar: TNewProgressBar;
  DesktopCheck: TNewCheckBox;

function RelaunchRequested: Boolean;
begin
  Result := ExpandConstant('{param:RELAUNCH|0}') = '1';
end;

function WantDesktopIcon: Boolean;
begin
  Result := DesktopCheck.Checked;
end;

procedure UpdateDiskPanel;
var
  Free, Total: Int64;
  Drive: String;
begin
  Drive := ExtractFileDrive(WizardDirValue);
  if (Drive <> '') and GetSpaceOnDisk64(Drive, Free, Total) and (Total > 0) then
  begin
    DiskLabel.Caption := Format('%s  -  %d GB free of %d GB.  Pacitum needs about 400 MB.', [Drive, Free div (1024*1024*1024), Total div (1024*1024*1024)]);
    DiskBar.Position := 100 - (Free * 100) div Total;
    DiskBar.Visible := True;
  end else begin
    DiskLabel.Caption := 'Pacitum needs about 400 MB of free disk space.';
    DiskBar.Visible := False;
  end;
end;

procedure DirChanged(Sender: TObject);
begin
  UpdateDiskPanel;
end;

procedure InitializeWizard;
begin
  WizardForm.WelcomeLabel1.Font.Size := 14;
  WizardForm.WelcomeLabel1.Font.Style := [fsBold];
  WizardForm.FinishedHeadingLabel.Font.Size := 14;
  WizardForm.FinishedHeadingLabel.Font.Style := [fsBold];
  WizardForm.PageNameLabel.Font.Style := [fsBold];
  WizardForm.NextButton.Height := ScaleY(28);
  WizardForm.BackButton.Height := ScaleY(28);
  WizardForm.CancelButton.Height := ScaleY(28);

  { Disk-space panel on the location page (mockup screen 2) }
  WizardForm.DiskSpaceLabel.Visible := False;
  DiskLabel := TNewStaticText.Create(WizardForm);
  DiskLabel.Parent := WizardForm.SelectDirPage;
  DiskLabel.Left := WizardForm.DirEdit.Left;
  DiskLabel.Top := WizardForm.DirEdit.Top + WizardForm.DirEdit.Height + ScaleY(24);
  DiskLabel.Width := WizardForm.DirEdit.Width;
  DiskLabel.AutoSize := False;
  DiskLabel.Height := ScaleY(16);
  DiskBar := TNewProgressBar.Create(WizardForm);
  DiskBar.Parent := WizardForm.SelectDirPage;
  DiskBar.Left := WizardForm.DirEdit.Left;
  DiskBar.Top := DiskLabel.Top + DiskLabel.Height + ScaleY(6);
  DiskBar.Width := WizardForm.DirEdit.Width;
  DiskBar.Height := ScaleY(6);
  DiskBar.Min := 0;
  DiskBar.Max := 100;
  WizardForm.DirEdit.OnChange := @DirChanged;

  DesktopCheck := TNewCheckBox.Create(WizardForm);
  DesktopCheck.Parent := WizardForm.SelectDirPage;
  DesktopCheck.Left := WizardForm.DirEdit.Left;
  DesktopCheck.Top := DiskBar.Top + DiskBar.Height + ScaleY(22);
  DesktopCheck.Width := WizardForm.DirEdit.Width;
  DesktopCheck.Height := ScaleY(20);
  DesktopCheck.Caption := 'Create a desktop shortcut';
  { Fresh install: on. Upgrade (directory page skipped): keep whatever the user had before. }
  if WizardForm.PrevAppDir = '' then
    DesktopCheck.Checked := True
  else
    DesktopCheck.Checked := FileExists(ExpandConstant('{autodesktop}\Pacitum Browser.lnk'));
end;

procedure CurPageChanged(CurPageID: Integer);
begin
  if CurPageID = wpSelectDir then
  begin
    UpdateDiskPanel;
  end;
end;
