; SPDX-License-Identifier: MPL-2.0
; This Source Code Form is subject to the terms of the Mozilla Public License v. 2.0. See https://mozilla.org/MPL/2.0.
#define AppVersion "1.3.0"
#define SdkDir "..\dist\Kirton-Compiler-1.3.0-windows-x64"

[Setup]
AppId={{C623E02D-CA91-4C28-83CE-AFF04C215892}
AppName=Kirton SDK
AppVersion={#AppVersion}
DefaultDirName={localappdata}\Programs\Kirton SDK
DefaultGroupName=Kirton SDK
LicenseFile={#SdkDir}\LICENSE
PrivilegesRequired=lowest
ArchitecturesInstallIn64BitMode=x64
OutputDir=..dist
OutputBaseFilename=Kirton-Compiler-1.3.0-windows-x64-setup-ru-en
UninstallDisplayName=Kirton SDK {#AppVersion}
ChangesEnvironment=yes
WizardStyle=modern

[Tasks]
Name: "addpath"; Description: "Add Kirton bin directory to your user PATH"; Flags: unchecked

[Files]
Source: "{#SdkDir}\*"; DestDir: "{app}"; Flags: recursesubdirs ignoreversion

[Code]
procedure AddUserPath;
var Existing, BinDir: string;
begin
  BinDir := ExpandConstant('{app}\bin');
  if not RegQueryStringValue(HKCU, 'Environment', 'Path', Existing) then
    Existing := '';
  if Pos(Uppercase(BinDir), Uppercase(Existing)) = 0 then begin
    if (Existing <> '') and (Existing[Length(Existing)] <> ';') then
      Existing := Existing + ';';
    RegWriteStringValue(HKCU, 'Environment', 'Path', Existing + BinDir);
    RegWriteStringValue(HKCU, 'Software\KirtonSDK', 'PathAdded', BinDir);
  end;
end;

procedure RemoveUserPath;
var Existing, BinDir, AddedPath: string;
begin
  if RegQueryStringValue(HKCU, 'Software\KirtonSDK', 'PathAdded', AddedPath) and
     RegQueryStringValue(HKCU, 'Environment', 'Path', Existing) then begin
    BinDir := AddedPath;
    StringChangeEx(Existing, BinDir + ';', '', True);
    StringChangeEx(Existing, ';' + BinDir, '', True);
    if CompareText(Existing, BinDir) = 0 then Existing := '';
    if Existing = '' then
      RegDeleteValue(HKCU, 'Environment', 'Path')
    else
      RegWriteStringValue(HKCU, 'Environment', 'Path', Existing);
    RegDeleteValue(HKCU, 'Software\KirtonSDK', 'PathAdded');
  end;
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if (CurStep = ssPostInstall) and WizardIsTaskSelected('addpath') then
    AddUserPath;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usUninstall then
    RemoveUserPath;
end;
