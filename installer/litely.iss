; Per-user installer for Litely. Built by installer\build.cmd; /DTest makes a separately named copy for tests.
#define AppVersion GetFileProductVersion("..\build\Litely.exe")
#ifdef Test
  #define AppName "Litely Test"
  #define AppId "{{470EFAE4-E20A-445D-B472-66D25FC555AF}"
  #define RunKey "Software\LitelyTest\Run"
  #define DirName "LitelyTest"
  #define SetupName "Litely-Test-Setup"
#else
  #define AppName "Litely"
  #define AppId "{{354B64EB-840A-4811-B5A5-76DE20AB14A7}"
  #define RunKey "Software\Microsoft\Windows\CurrentVersion\Run"
  #define DirName "Litely"
  #define SetupName "Litely-Setup"
#endif
; Sample wallpapers are whatever MP4s are in SamplesDir; with none, the option is left out.
#ifndef SamplesDir
  #define SamplesDir "..\build\wallpapers"
#endif
#define SampleBytes 0
#define FindHandle
#define FindResult
#sub AddSample
  #expr SampleBytes = SampleBytes + FileSize(SamplesDir + "\" + FindGetFileName(FindHandle))
#endsub
#for {FindHandle = FindResult = FindFirst(SamplesDir + "\*.mp4", 0); FindResult; FindResult = FindNext(FindHandle)} AddSample
#if FindHandle
  #expr FindClose(FindHandle)
#endif

[Setup]
AppId={#AppId}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher=Litely
DefaultDirName={localappdata}\Programs\{#DirName}
DisableProgramGroupPage=yes
DisableDirPage=yes
PrivilegesRequired=lowest
UsePreviousTasks=no
CloseApplications=no
MinVersion=10.0
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir=..\build\installer
OutputBaseFilename={#SetupName}
SetupIconFile=..\assets\litely.ico
UninstallDisplayIcon={app}\Litely.exe
UninstallDisplayName={#AppName}
WizardStyle=modern
Compression=lzma2
SolidCompression=yes

[Tasks]
Name: startup; Description: "Start Litely with Windows"
#if SampleBytes > 0
Name: samples; Description: "Include sample wallpapers ({#(SampleBytes + 524288) / 1048576} MB)"
#endif

[Files]
Source: "..\build\Litely.exe"; DestDir: "{app}"; Flags: ignoreversion
#if SampleBytes > 0
Source: "{#SamplesDir}\*.mp4"; DestDir: "{app}\wallpapers"; Tasks: samples; Flags: ignoreversion
#endif

[Icons]
Name: "{autoprograms}\{#AppName}"; Filename: "{app}\Litely.exe"

[Registry]
Root: HKCU; Subkey: "{#RunKey}"; ValueType: string; ValueName: "Litely"; ValueData: """{app}\Litely.exe"" --startup"; Tasks: startup; Flags: uninsdeletevalue

[Run]
Filename: "{app}\Litely.exe"; Description: "Launch Litely"; Flags: nowait postinstall skipifsilent

[Code]
// Closing Litely's hidden controller window makes it save its state and exit.
procedure CloseLitely;
var
  I: Integer;
begin
  for I := 1 to 50 do begin
    if FindWindowByClassName('LitelyController') = 0 then
      Exit;
    PostMessage(FindWindowByClassName('LitelyController'), 16, 0, 0);
    Sleep(100);
  end;
end;

var
  TasksShown: Boolean;

function GetWindowLong(Wnd: HWND; Index: Integer): Longint; external 'GetWindowLongW@user32.dll stdcall';
function SetWindowLong(Wnd: HWND; Index: Integer; NewLong: Longint): Longint; external 'SetWindowLongW@user32.dll stdcall';

// On an upgrade the startup box follows the current setting, which Litely's Settings may have changed.
// Silent installs pass through this page too, so an explicit /TASKS wins.
procedure CurPageChanged(CurPageID: Integer);
begin
  // The task list blanks each box before redrawing it on hover; composited painting hides that blank frame.
  if CurPageID = wpSelectTasks then
    SetWindowLong(WizardForm.TasksList.Handle, -20, GetWindowLong(WizardForm.TasksList.Handle, -20) or $02000000);
  if (CurPageID = wpSelectTasks) and not TasksShown and (ExpandConstant('{param:TASKS|}') = '') then begin
    TasksShown := True;
    if (WizardForm.PrevAppDir <> '') and not RegValueExists(HKCU, '{#RunKey}', 'Litely') then
      WizardSelectTasks('!startup');
  end;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  CloseLitely;
  Result := '';
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if (CurStep = ssPostInstall) and not WizardIsTaskSelected('startup') then
    RegDeleteValue(HKCU, '{#RunKey}', 'Litely');
end;

// One question with a box for the data, in place of Inno's plain Yes/No. The answer reruns the uninstaller
// quietly, because the built-in confirmation can't be replaced otherwise.
function InitializeUninstall(): Boolean;
var
  Form: TSetupForm;
  Question: TNewStaticText;
  Data: TNewCheckBox;
  Remove, Keep: TNewButton;
  Code: Integer;
begin
  Result := True;
  if UninstallSilent then begin
    CloseLitely;
    Exit;
  end;
  Result := False;
  Form := CreateCustomForm(400, 124, False, True);
  try
    Form.Caption := 'Uninstall {#AppName}';
    Question := TNewStaticText.Create(Form);
    Question.Parent := Form;
    Question.Left := ScaleX(16);
    Question.Top := ScaleY(16);
    Question.Caption := 'Remove {#AppName} from this PC?';
    Data := TNewCheckBox.Create(Form);
    Data.Parent := Form;
    Data.Left := ScaleX(16);
    Data.Top := ScaleY(44);
    Data.Width := Form.ClientWidth - ScaleX(32);
    Data.Height := ScaleY(17);
    Data.Caption := 'Also delete my Library, settings and converted wallpapers';
    Remove := TNewButton.Create(Form);
    Remove.Parent := Form;
    Remove.Caption := 'Uninstall';
    Remove.ModalResult := mrOk;
    Remove.Default := True;
    Keep := TNewButton.Create(Form);
    Keep.Parent := Form;
    Keep.Caption := 'Cancel';
    Keep.ModalResult := mrCancel;
    Keep.Cancel := True;
    Remove.Width := Form.CalculateButtonWidth([Remove.Caption, Keep.Caption]);
    Keep.Width := Remove.Width;
    Remove.Height := ScaleY(23);
    Keep.Height := ScaleY(23);
    Keep.Left := Form.ClientWidth - Keep.Width - ScaleX(12);
    Remove.Left := Keep.Left - Remove.Width - ScaleX(6);
    Remove.Top := Form.ClientHeight - ScaleY(23 + 12);
    Keep.Top := Remove.Top;
    Form.ActiveControl := Keep;
    if Form.ShowModal() = mrOk then
      Exec(ExpandConstant('{uninstallexe}'), '/SILENT /DELETEDATA=' + IntToStr(Ord(Data.Checked)), '', SW_SHOW, ewNoWait, Code);
  finally
    Form.Free();
  end;
end;

// Litely keeps its data where LOCALAPPDATA points, as Litely itself does; /DELETEDATA=1 removes it.
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  Data: String;
begin
  Data := GetEnv('LOCALAPPDATA') + '\Litely';
  if (CurUninstallStep = usPostUninstall) and DirExists(Data) and (ExpandConstant('{param:DELETEDATA|0}') = '1') then
    DelTree(Data, True, True, True);
end;
