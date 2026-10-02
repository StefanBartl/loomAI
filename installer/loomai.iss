; loomAI Windows installer (Inno Setup 6), built by .github/workflows/release.yml.
;
; It packs the output of scripts/setup.ps1 -- <StageDir>\app (binary, runtime
; DLLs, dashboard) and <StageDir>\loomai.cmd -- so an installed copy has exactly
; the layout a setup.ps1 install has. The server serves ./dashboard relative to
; its working directory; the Start-menu shortcut and loomai.cmd both start it
; from <app>\app.
;
; Per-user install (no admin rights, no UAC prompt). The optional "add to PATH"
; task edits the user PATH only and undoes that on uninstall.
;
; Build locally:
;   ISCC.exe /DAppVersion=0.1.0 /DStageDir=C:\path\to\stage /ODist installer\loomai.iss

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef StageDir
  #error StageDir is not defined: pass /DStageDir=<output of scripts/setup.ps1 -Prefix ...>
#endif

[Setup]
; Never change AppId: it is how a newer installer finds and upgrades an installed copy.
AppId={{5EA66451-99A0-4CBF-8A48-261C812EA959}
AppName=loomAI
AppVersion={#AppVersion}
AppVerName=loomAI {#AppVersion}
AppPublisher=Stefan Bartl
AppPublisherURL=https://github.com/StefanBartl/loomAI
AppSupportURL=https://github.com/StefanBartl/loomAI/issues
AppUpdatesURL=https://github.com/StefanBartl/loomAI/releases
VersionInfoVersion={#AppVersion}
DefaultDirName={autopf}\loomAI
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputBaseFilename=loomai-setup-{#AppVersion}-x64
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
ChangesEnvironment=yes
UninstallDisplayIcon={app}\app\loomai.exe

[Tasks]
Name: "addtopath"; Description: "Add loomAI to the user &PATH (the ""loomai"" command in new terminals)"; Flags: unchecked

[Files]
Source: "{#StageDir}\app\*"; DestDir: "{app}\app"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#StageDir}\loomai.cmd"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\loomAI"; Filename: "{app}\app\loomai.exe"; WorkingDir: "{app}\app"; Comment: "Start the local loomAI server on 127.0.0.1:8080"

[Run]
Filename: "{app}\app\loomai.exe"; WorkingDir: "{app}\app"; Description: "Start loomAI now"; Flags: postinstall nowait skipifsilent unchecked

[Code]
const
  EnvKey = 'Environment';

function SamePath(const A, B: string): Boolean;
begin
  Result := CompareText(RemoveBackslashUnlessRoot(A), RemoveBackslashUnlessRoot(B)) = 0;
end;

{ True if Dir is one of the ';'-separated entries of Path. }
function PathContains(const Path, Dir: string): Boolean;
var
  Rest, Item: string;
  P: Integer;
begin
  Result := False;
  Rest := Path;
  while Rest <> '' do
  begin
    P := Pos(';', Rest);
    if P = 0 then
    begin
      Item := Rest;
      Rest := '';
    end
    else
    begin
      Item := Copy(Rest, 1, P - 1);
      Rest := Copy(Rest, P + 1, MaxInt);
    end;
    if (Item <> '') and SamePath(Item, Dir) then
    begin
      Result := True;
      Exit;
    end;
  end;
end;

{ Path is read and written unexpanded (RegQueryStringValue does not expand
  REG_EXPAND_SZ), so other entries keep their %VAR% references. }
procedure AddToUserPath(const Dir: string);
var
  Path: string;
begin
  if not RegQueryStringValue(HKCU, EnvKey, 'Path', Path) then
    Path := '';
  if PathContains(Path, Dir) then
    Exit;
  if (Path <> '') and (Path[Length(Path)] <> ';') then
    Path := Path + ';';
  RegWriteExpandStringValue(HKCU, EnvKey, 'Path', Path + Dir);
end;

procedure RemoveFromUserPath(const Dir: string);
var
  Path, Rest, Item, NewPath: string;
  P: Integer;
begin
  if not RegQueryStringValue(HKCU, EnvKey, 'Path', Path) then
    Exit;
  Rest := Path;
  NewPath := '';
  while Rest <> '' do
  begin
    P := Pos(';', Rest);
    if P = 0 then
    begin
      Item := Rest;
      Rest := '';
    end
    else
    begin
      Item := Copy(Rest, 1, P - 1);
      Rest := Copy(Rest, P + 1, MaxInt);
    end;
    if (Item <> '') and (not SamePath(Item, Dir)) then
    begin
      if NewPath <> '' then
        NewPath := NewPath + ';';
      NewPath := NewPath + Item;
    end;
  end;
  if NewPath <> Path then
    RegWriteExpandStringValue(HKCU, EnvKey, 'Path', NewPath);
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if (CurStep = ssPostInstall) and WizardIsTaskSelected('addtopath') then
    AddToUserPath(ExpandConstant('{app}'));
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usPostUninstall then
    RemoveFromUserPath(ExpandConstant('{app}'));
end;
