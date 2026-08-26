#ifndef Configuration
  #define Configuration "Release"
#endif

#define AppName "CodexQuotaTray"
#define AppVersion "1.0.0"
#define RepoRoot SourcePath + "..\"
#define AppExe RepoRoot + "build\x64-release\" + Configuration + "\CodexQuotaTray.exe"

[Setup]
AppId={{A3E9322D-E185-4DCE-921F-F6F984922563}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher=CodexQuotaTray
DefaultDirName={localappdata}\Programs\CodexQuotaTray
DefaultGroupName=CodexQuotaTray
OutputDir={#RepoRoot}dist
OutputBaseFilename=CodexQuotaTray-Setup-{#AppVersion}-x64
Compression=lzma2
SolidCompression=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
UninstallDisplayIcon={app}\CodexQuotaTray.exe
WizardStyle=modern
SetupLogging=yes
AppMutex=Local\CodexQuotaTray.Singleton.v1
CloseApplications=force
RestartApplications=no

[Tasks]
Name: "startup"; Description: "随 Windows 启动"; GroupDescription: "其他选项:"; Flags: unchecked

[Files]
Source: "{#AppExe}"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\CodexQuotaTray"; Filename: "{app}\CodexQuotaTray.exe"

[Registry]
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "CodexQuotaTray"; ValueData: """{app}\CodexQuotaTray.exe"" --background"; Tasks: startup; Flags: uninsdeletevalue

[Run]
Filename: "{app}\CodexQuotaTray.exe"; Description: "启动 CodexQuotaTray"; Flags: nowait postinstall skipifsilent

[Code]
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usUninstall then
    RegDeleteValue(HKCU, 'Software\Microsoft\Windows\CurrentVersion\Run', 'CodexQuotaTray');
end;
