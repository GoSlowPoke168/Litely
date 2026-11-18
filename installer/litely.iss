; Per-user installer for Litely. Built by installer\build.cmd; /DTest makes a separately named copy for tests.
#define AppVersion "0.1.0"
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
Name: samples; Description: "Include sample wallpapers (18 MB)"

[Files]
Source: "..\build\Litely.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\build\wallpapers\*.mp4"; DestDir: "{app}\wallpapers"; Tasks: samples; Flags: ignoreversion

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

// On an upgrade the startup box follows the current setting, which Litely's Settings may have changed.
// Silent installs pass through this page too, so an explicit /TASKS wins.
procedure CurPageChanged(CurPageID: Integer);
begin
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

function InitializeUninstall(): Boolean;
begin
  CloseLitely;
  Result := True;
end;

// Litely keeps its data where LOCALAPPDATA points, as Litely itself does. Silent uninstalls keep it unless /DELETEDATA=1.
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  Data: String;
begin
  Data := GetEnv('LOCALAPPDATA') + '\Litely';
  if (CurUninstallStep = usPostUninstall) and DirExists(Data) then
    if (ExpandConstant('{param:DELETEDATA|0}') = '1') or
       (not UninstallSilent and (MsgBox('Also delete your Litely Library, settings and converted copies?', mbConfirmation, MB_YESNO or MB_DEFBUTTON2) = IDYES)) then
      DelTree(Data, True, True, True);
end;
