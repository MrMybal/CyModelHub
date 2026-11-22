; SPDX-FileCopyrightText: 2026 Cyberalien
; SPDX-License-Identifier: GPL-3.0-only

[Setup]
AppId={{DBF8620E-B7CE-46FC-8420-480D64F2FD50}
AppName=CyModelHub
AppVersion={#AppVersion}
AppPublisher=Cyberalien
AppPublisherURL=https://github.com/MrMybal/CyModelHub
AppUpdatesURL=https://github.com/MrMybal/CyModelHub/releases
DefaultDirName={localappdata}\Programs\CyModelHub
DefaultGroupName=CyModelHub
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
SetupMutex=Local\CyModelHub.Setup
CloseApplications=no
RestartApplications=no
UninstallDisplayIcon={app}\CyModelHub.exe
SetupIconFile=..\..\assets\branding\cymodelhub.ico
OutputBaseFilename=CyModelHub-{#AppVersion}-windows-x64-setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "french"; MessagesFile: "compiler:Languages\French.isl"

[CustomMessages]
english.LaunchApp=Launch CyModelHub
french.LaunchApp=Lancer CyModelHub
english.WaitForApp=Quit CyModelHub from its notification area menu, then run Setup again.
french.WaitForApp=Quittez CyModelHub depuis son menu de notification, puis relancez le programme d’installation.
english.WaitForService=Finish running jobs and stop any older CyModelHub service before trying again.
french.WaitForService=Attendez la fin des tâches et arrêtez les anciennes instances du service CyModelHub avant de réessayer.
english.CannotLock=Could not prepare the CyModelHub installation.
french.CannotLock=Impossible de préparer l’installation de CyModelHub.

[Files]
Source: "{#StagedFiles}\CyModelHubCli.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#StagedFiles}\msvcp140.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#StagedFiles}\msvcp140_atomic_wait.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#StagedFiles}\vcruntime140.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#StagedFiles}\vcruntime140_1.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#StagedFiles}\*"; DestDir: "{app}"; Excludes: "CyModelHubCli.exe,msvcp140.dll,msvcp140_atomic_wait.dll,vcruntime140.dll,vcruntime140_1.dll"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\CyModelHub"; Filename: "{app}\CyModelHub.exe"

[Run]
Filename: "{app}\CyModelHub.exe"; Parameters: "{code:ProfileArguments}"; Description: "{cm:LaunchApp}"; Flags: nowait postinstall skipifsilent

[Code]
const
  DesktopMarker = 'Local\CyModelHub.Desktop';
  ServiceMarker = 'Local\CyModelHub.Service';
  InstallationMarker = 'Local\CyModelHub.Installing';
  WindowsRunKey = 'Software\Microsoft\Windows\CurrentVersion\Run';
  WindowsRunName = 'CyModelHub';
  WindowsPreferencesKey = 'Software\CyModelHub';
  WindowsStartupConfigured = 'StartupConfigured';
  Synchronize = $00100000;
  WaitObject = 0;
var
  InstallLock: LongWord;

function OpenProcess(Access: LongWord; InheritHandle: Boolean; Pid: LongWord): LongWord;
  external 'OpenProcess@kernel32.dll stdcall';
function WaitForSingleObject(Handle: LongWord; Milliseconds: LongWord): LongWord;
  external 'WaitForSingleObject@kernel32.dll stdcall';
function CreateMutex(Attributes: LongWord; Owner: Boolean; Name: String): LongWord;
  external 'CreateMutexW@kernel32.dll stdcall';
function CloseHandle(Handle: LongWord): Boolean;
  external 'CloseHandle@kernel32.dll stdcall';

function ProfileArguments(Param: String): String;
var
  Directory: String;
begin
  Directory := ExpandConstant('{param:CYMODELHUBDATA|}');
  Result := '';
  if Directory <> '' then
    Result := '--data ' + AddQuotes(RemoveBackslashUnlessRoot(Directory));
end;

function InitializeSetup(): Boolean;
var
  Pid: Integer;
  Parent: LongWord;
begin
  Result := True;
  Pid := StrToIntDef(ExpandConstant('{param:CYMODELHUBAPP|0}'), 0);
  if Pid <= 0 then
    Exit;
  Parent := OpenProcess(Synchronize, False, Pid);
  if Parent = 0 then
    Exit;
  Result := WaitForSingleObject(Parent, 20000) = WaitObject;
  CloseHandle(Parent);
  if not Result then
    SuppressibleMsgBox(CustomMessage('WaitForApp'), mbError, MB_OK, IDOK);
end;

procedure ReleaseInstallLock();
begin
  if InstallLock <> 0 then
  begin
    CloseHandle(InstallLock);
    InstallLock := 0;
  end;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  ExitCode, Attempt: Integer;
begin
  Result := CustomMessage('CannotLock');
  ReleaseInstallLock();
  InstallLock := CreateMutex(0, False, InstallationMarker);
  if InstallLock = 0 then
    Exit;

  if CheckForMutexes(DesktopMarker) then
    Result := CustomMessage('WaitForApp')
  else
  begin
    ExtractTemporaryFile('CyModelHubCli.exe');
    ExtractTemporaryFile('msvcp140.dll');
    ExtractTemporaryFile('msvcp140_atomic_wait.dll');
    ExtractTemporaryFile('vcruntime140.dll');
    ExtractTemporaryFile('vcruntime140_1.dll');
    Result := CustomMessage('WaitForService');
    if Exec(ExpandConstant('{tmp}\CyModelHubCli.exe'), ProfileArguments('') + ' stop-for-update',
      ExpandConstant('{tmp}'), SW_HIDE, ewWaitUntilTerminated, ExitCode) and (ExitCode = 0) then
    begin
      for Attempt := 1 to 20 do
      begin
        if not CheckForMutexes(ServiceMarker) then
        begin
          Result := '';
          Break;
        end;
        Sleep(100);
      end;
    end;
  end;
  if Result <> '' then
    ReleaseInstallLock();
end;

function WindowsStartupCommand(): String;
begin
  Result := '"' + ExpandConstant('{app}\CyModelHub.exe') + '" --start-in-tray';
end;

procedure ConfigureWindowsStartup();
var
  Command, Profile: String;
begin
  if RegValueExists(HKCU, WindowsPreferencesKey, WindowsStartupConfigured) then
    Exit;
  if not RegValueExists(HKCU, WindowsRunKey, WindowsRunName) then
  begin
    Command := WindowsStartupCommand();
    Profile := ProfileArguments('');
    if Profile <> '' then
      Command := Command + ' ' + Profile;
    if Length(Command) > 260 then
      Exit;
    if not RegWriteStringValue(HKCU, WindowsRunKey, WindowsRunName, Command) then
      Exit;
  end;
  RegWriteDWordValue(HKCU, WindowsPreferencesKey, WindowsStartupConfigured, 1);
end;

procedure CurStepChanged(Step: TSetupStep);
begin
  if Step = ssPostInstall then
  begin
    ConfigureWindowsStartup();
    ReleaseInstallLock();
  end;
end;

procedure DeinitializeSetup();
begin
  ReleaseInstallLock();
end;

function InitializeUninstall(): Boolean;
begin
  Result := not CheckForMutexes(DesktopMarker + ',' + ServiceMarker);
  if not Result then
    SuppressibleMsgBox(CustomMessage('WaitForApp') + #13#10 + CustomMessage('WaitForService'),
      mbError, MB_OK, IDOK);
end;

procedure CurUninstallStepChanged(Step: TUninstallStep);
var
  Command, TrayCommand: String;
begin
  if Step <> usPostUninstall then
    Exit;
  TrayCommand := WindowsStartupCommand();
  if RegQueryStringValue(HKCU, WindowsRunKey, WindowsRunName, Command) and
    ((CompareText(Command, TrayCommand) = 0) or
     (CompareText(Copy(Command, 1, Length(TrayCommand) + 8), TrayCommand + ' --data ') = 0)) then
    if RegDeleteValue(HKCU, WindowsRunKey, WindowsRunName) then
      RegDeleteValue(HKCU, WindowsPreferencesKey, WindowsStartupConfigured);
end;
