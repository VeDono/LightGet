; LightGet — Windows installer (Inno Setup 6).
;
; Stable AppId => installing a NEWER version over an older one UPGRADES it in
; place (Inno finds the existing install via the AppId and replaces the files,
; no duplicate "LightGet (2)" install). CloseApplications=yes closes the running
; tray app during the upgrade so its files can be replaced.
;
; Built in CI with:
;   ISCC /DMyAppVersion=<ver> /DDistDir=<abs path to windeployqt output> LightGet.iss
; Output: Output\LightGet-Setup-Windows-x64.exe

#ifndef MyAppVersion
  #define MyAppVersion "1.0.5"
#endif
#ifndef DistDir
  #define DistDir "dist"
#endif
; SetupIconFile is resolved relative to THIS .iss file.
#ifndef SetupIcon
  #define SetupIcon "..\..\resources\AppIcon.ico"
#endif

[Setup]
AppId={{B4E7C2A1-9D3F-4E58-A6C0-1F2D3E4A5B6C}
AppName=LightGet
AppVersion={#MyAppVersion}
AppVerName=LightGet {#MyAppVersion}
AppPublisher=Sergey Emelyanov
AppPublisherURL=https://github.com/VeDono/LightGet
DefaultDirName={autopf}\LightGet
DisableProgramGroupPage=yes
DisableDirPage=auto
UninstallDisplayIcon={app}\LightGet.exe
UninstallDisplayName=LightGet
OutputDir=Output
OutputBaseFilename=LightGet-Setup-Windows-x64
SetupIconFile={#SetupIcon}
Compression=lzma2
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
WizardStyle=modern
; Close a running LightGet (tray app) during install/upgrade, then it can launch
; again from the [Run] section. Do not force a reboot.
CloseApplications=yes
RestartApplications=no

[Languages]
Name: "en"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "autostart"; Description: "Start LightGet when Windows starts"; GroupDescription: "Startup options:"; Flags: unchecked

[Files]
; The whole windeployqt output folder (LightGet.exe + Qt DLLs + plugins).
Source: "{#DistDir}\*"; DestDir: "{app}"; Flags: recursesubdirs createallsubdirs ignoreversion

[Icons]
Name: "{autoprograms}\LightGet"; Filename: "{app}\LightGet.exe"
Name: "{userstartup}\LightGet"; Filename: "{app}\LightGet.exe"; Tasks: autostart

[Run]
Filename: "{app}\LightGet.exe"; Description: "Launch LightGet now"; Flags: nowait postinstall skipifsilent
