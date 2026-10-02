; Inno Setup script for the Windows installer.
; Build: iscc installer\mouse-hardware-fix.iss  (after building the Release exe)
; Override with /DAppVersion=x.y.z and /DSourceExe=path\to\mouse-hardware-fix.exe

#ifndef AppVersion
  #define AppVersion "1.0.0"
#endif
#ifndef SourceExe
  #define SourceExe "..\build\Release\mouse-hardware-fix.exe"
#endif

[Setup]
AppId={{00290A29-D1EB-4F19-B4E8-04EC52AE894F}
AppName=mouse-hardware-fix
AppVersion={#AppVersion}
AppPublisher=Aleynikovich
AppPublisherURL=https://github.com/Aleynikovich/mouse-hardware-fix
DefaultDirName={autopf}\mouse-hardware-fix
DisableProgramGroupPage=yes
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
OutputDir=..\build
OutputBaseFilename=mouse-hardware-fix-setup
UninstallDisplayIcon={app}\mouse-hardware-fix.exe
Compression=lzma2
SolidCompression=yes
WizardStyle=modern

[Files]
Source: "{#SourceExe}"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\README.md"; DestDir: "{app}"; Flags: ignoreversion

[UninstallRun]
; Removes the logon task and stops the instance in this session; taskkill
; catches instances in other users' sessions so the exe can be deleted.
Filename: "{app}\mouse-hardware-fix.exe"; Parameters: "--uninstall"; Flags: runhidden waituntilterminated; RunOnceId: "RemoveTask"
Filename: "{sys}\taskkill.exe"; Parameters: "/F /IM mouse-hardware-fix.exe"; Flags: runhidden waituntilterminated; RunOnceId: "KillAll"

[Code]
var
  OptionsPage: TInputQueryWizardPage;

procedure InitializeWizard;
begin
  OptionsPage := CreateInputQueryPage(wpSelectDir, 'Options',
    'Command-line options for mouse-hardware-fix',
    'Leave empty for the defaults. Examples: --click-timeout 0.035, --disable-scroll, ' +
    '--scroll-drop. See README.md in the install folder for the full list.');
  OptionsPage.Add('Options:', False);
  OptionsPage.Values[0] := GetPreviousData('Options', '');
end;

procedure RegisterPreviousData(PreviousDataKey: Integer);
begin
  SetPreviousData(PreviousDataKey, 'Options', OptionsPage.Values[0]);
end;

// Stop a running copy so its exe can be replaced.
function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  Code: Integer;
  Exe: String;
begin
  Exe := ExpandConstant('{app}\mouse-hardware-fix.exe');
  if FileExists(Exe) then
    Exec(Exe, '--stop', '', SW_HIDE, ewWaitUntilTerminated, Code);
  Exec(ExpandConstant('{sys}\taskkill.exe'), '/F /IM mouse-hardware-fix.exe', '', SW_HIDE, ewWaitUntilTerminated, Code);
  Result := '';
end;

// Register the logon task and start it now.
procedure CurStepChanged(CurStep: TSetupStep);
var
  Code: Integer;
begin
  if CurStep <> ssPostInstall then Exit;
  if not Exec(ExpandConstant('{app}\mouse-hardware-fix.exe'), '--install ' + Trim(OptionsPage.Values[0]),
              '', SW_HIDE, ewWaitUntilTerminated, Code) or (Code <> 0) then
    MsgBox('mouse-hardware-fix could not be registered to start at logon. ' +
           'Check the options you entered, then run setup again.', mbError, MB_OK);
end;
