#ifndef AppVersion
  #error AppVersion must be supplied by package-installer.ps1.
#endif
#ifndef AppFileVersion
  #error AppFileVersion must be supplied by package-installer.ps1.
#endif
#ifndef PackageDirectory
  #error PackageDirectory must be supplied by package-installer.ps1.
#endif
#ifndef OutputDirectory
  #error OutputDirectory must be supplied by package-installer.ps1.
#endif
#if Ver != EncodeVer(6, 5, 4)
  #error Inno Setup 6.5.4 is required.
#endif

[Setup]
AppId={{9F5A36F8-2E37-4D4E-9838-580B3BEE3C21}
AppName=XMJD
AppVersion={#AppVersion}
AppVerName=XMJD {#AppVersion}
AppPublisher=XMJD
VersionInfoVersion={#AppFileVersion}
DefaultDirName={autopf}\XMJD\{#AppVersion}
DisableDirPage=yes
UsePreviousAppDir=no
DisableProgramGroupPage=yes
UsePreviousGroup=no
PrivilegesRequired=admin
ArchitecturesAllowed=x64os
ArchitecturesInstallIn64BitMode=x64os
MinVersion=10.0.22000
OutputDir={#OutputDirectory}
OutputBaseFilename=XMJD-{#AppVersion}-windows-x64-setup
SetupIconFile=..\Resources\XMJD.ico
UninstallDisplayIcon={app}\XMJDHost.exe
LicenseFile=..\LICENSE.txt
WizardStyle=modern
Compression=lzma2
SolidCompression=yes
CloseApplications=no
RestartApplications=no
AlwaysRestart=no
RestartIfNeededByRun=no
AllowCancelDuringInstall=no
SetupLogging=yes
UninstallLogging=yes
SetupMutex=XMJDSetup,Global\XMJDSetup

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Files]
Source: "{#PackageDirectory}\manifest.json"; DestName: "XMJD-repair-manifest.json"; Flags: dontcopy
Source: "{#PackageDirectory}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs uninsrestartdelete

[Icons]
Name: "{autoprograms}\星猫键道\星猫键道设置"; Filename: "{app}\XMJDSettings.exe"; WorkingDir: "{app}"
Name: "{autoprograms}\星猫键道\卸载星猫键道"; Filename: "{uninstallexe}"

[Run]
Filename: "{app}\XMJDSettings.exe"; Description: "打开星猫键道设置并选择输入方案"; Flags: postinstall nowait skipifsilent runasoriginaluser

[Messages]
FinishedLabel=XMJD has been installed and registered for x64 and x86 applications.%n%nUse the Start Menu shortcut "星猫键道设置" to choose an input scheme. Then add XMJD in Windows Settings and press Win+Space. The Host starts in your normal desktop session when XMJD is activated or at your next sign-in. Your default input method was not changed.
ConfirmUninstall=Remove XMJD and its x64/x86 input-method registration?%n%nYour local XMJD user data will be preserved. Applications using the input method will not be closed. A restart may be required to remove loaded program files.

[Code]
const
  TipClassKey = 'Software\Classes\CLSID\{5F8F59F4-E5A8-4944-960B-8A8AD3BE8857}\InprocServer32';
  StartupKey = 'Software\Microsoft\Windows\CurrentVersion\Run';
  UninstallKey = 'Software\Microsoft\Windows\CurrentVersion\Uninstall\{9F5A36F8-2E37-4D4E-9838-580B3BEE3C21}_is1';
  FileAttributeReparsePoint = $400;
  InvalidFileAttributes = $FFFFFFFF;

var
  InstallFailureCode: Integer;
  InstallFailureMessage: String;
  RepairMode: Boolean;

function GetFileAttributesW(lpFileName: String): Cardinal;
  external 'GetFileAttributesW@kernel32.dll stdcall';

function FixedInstallDirectory: String;
begin
  Result := ExpandConstant('{autopf}\XMJD\{#AppVersion}');
end;

function HasReparsePoint(const Directory: String): Boolean;
var
  Attributes: Cardinal;
begin
  Attributes := GetFileAttributesW(Directory);
  Result := (Attributes <> InvalidFileAttributes) and
    ((Attributes and FileAttributeReparsePoint) <> 0);
end;

function RunPackageScript(const ScriptName, ArgumentName, ArgumentValue: String): Boolean;
var
  PowerShell, Parameters: String;
  ResultCode: Integer;
begin
  // x64 安装模式下 {sys} 指向原生 System32，避免从 32 位进程注册。
  PowerShell := ExpandConstant('{sys}\WindowsPowerShell\v1.0\powershell.exe');
  Parameters := '-NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "' +
    ExpandConstant('{app}\scripts\') + ScriptName + '" -' + ArgumentName +
    ' "' + ArgumentValue + '"';
  Result := False;
  try
    if ExecAndLogOutput(PowerShell, Parameters, ExpandConstant('{app}'),
      SW_SHOWNORMAL, ewWaitUntilTerminated, ResultCode, nil) then
    begin
      Log(Format('%s exit code: %d', [ScriptName, ResultCode]));
      Result := ResultCode = 0;
    end
    else
      Log(Format('Could not execute %s: %d', [ScriptName, ResultCode]));
  except
    Log('Package script failed: ' + GetExceptionMessage);
  end;
end;

function RepairRegistryValueMatches(RootKey: Integer; const Key, Name, Expected: String;
  Required: Boolean): Boolean;
var
  Actual: String;
begin
  Result := not Required;
  if RegValueExists(RootKey, Key, Name) then
    Result := RegQueryStringValue(RootKey, Key, Name, Actual) and
      (CompareText(Actual, Expected) = 0);
end;

function RepairOwnershipMatches: Boolean;
var
  Root, Host, Uninstaller: String;
begin
  Root := FixedInstallDirectory;
  Host := Root + '\XMJDHost.exe';
  Uninstaller := '"' + Root + '\unins000.exe"';
  Result := RegKeyExists(HKLM64, UninstallKey) and
    not RegKeyExists(HKLM32, UninstallKey) and
    not RegKeyExists(HKCU64, UninstallKey) and
    not RegKeyExists(HKCU32, UninstallKey) and
    RepairRegistryValueMatches(HKLM64, UninstallKey, 'DisplayVersion', '{#AppVersion}', True) and
    RepairRegistryValueMatches(HKLM64, UninstallKey, 'Inno Setup: App Path', Root, True) and
    RepairRegistryValueMatches(HKLM64, UninstallKey, 'InstallLocation', Root + '\', True) and
    RepairRegistryValueMatches(HKLM64, UninstallKey, 'UninstallString', Uninstaller + ' /LOG', True) and
    RepairRegistryValueMatches(HKLM64, UninstallKey, 'QuietUninstallString', Uninstaller + ' /SILENT /LOG', True) and
    RepairRegistryValueMatches(HKLM64, TipClassKey, '', Root + '\Tip\x64\XMJDTip.dll', False) and
    RepairRegistryValueMatches(HKLM32, TipClassKey, '', Root + '\Tip\x86\XMJDTip.dll', False) and
    RepairRegistryValueMatches(HKLM64, 'Software\XMJD', 'HostExecutablePath', Host, False) and
    RepairRegistryValueMatches(HKLM64, 'Software\XMJD', 'RegistrationComplete', Host, False) and
    RepairRegistryValueMatches(HKLM64, StartupKey, 'XMJDHost', '"' + Host + '"', False);
end;

function CheckRepairPayload(VerifyPayload: Boolean): Boolean;
var
  Script: TStringList;
  Parameters, ScriptPath: String;
  ResultCode: Integer;
begin
  { 只执行安装器生成的检查脚本；旧安装目录中的脚本在恢复前均不可信。 }
  ExtractTemporaryFile('XMJD-repair-manifest.json');
  ScriptPath := ExpandConstant('{tmp}\XMJD-repair-preflight.ps1');
  Script := TStringList.Create;
  try
    Script.Add('param([string] $Root, [string] $Version, [switch] $VerifyPayload)');
    Script.Add('$ErrorActionPreference = ''Stop''');
    Script.Add('function Assert-Manifest($Manifest) {');
    Script.Add('  if ($Manifest.formatVersion -ne 1 -or $Manifest.product -ne ''XMJD'' -or');
    Script.Add('      $Manifest.version -ne $Version -or $Manifest.configuration -ne ''Release'' -or');
    Script.Add('      ($Manifest.architectures -join '','') -ne ''x64,x86'') { throw ''Not a same-version XMJD Release manifest.'' }');
    Script.Add('}');
    Script.Add('$reference = Get-Content -LiteralPath (Join-Path $PSScriptRoot ''XMJD-repair-manifest.json'') -Raw | ConvertFrom-Json');
    Script.Add('Assert-Manifest $reference');
    Script.Add('$ancestor = Get-Item -LiteralPath $Root -Force');
    Script.Add('if (-not $ancestor.PSIsContainer) { throw ''Repair target is not a directory.'' }');
    Script.Add('while ($null -ne $ancestor) {');
    Script.Add('  if ($ancestor.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw ''Repair path contains a link.'' }');
    Script.Add('  $ancestor = $ancestor.Parent');
    Script.Add('}');
    Script.Add('$files = @{ ''manifest.json'' = $true; ''unins000.exe'' = $true; ''unins000.dat'' = $true; ''unins000.msg'' = $true }');
    Script.Add('$directories = @{}');
    Script.Add('foreach ($entry in $reference.files) {');
    Script.Add('  $relative = [string] $entry.path');
    Script.Add('  if ([string]::IsNullOrWhiteSpace($relative) -or $relative -match ''[\\:\x00-\x1f<>"|?*]'' -or');
    Script.Add('      $relative.StartsWith(''/'') -or $relative -match ''(^|/)(\.|\.\.|)(/|$)'' -or $files.ContainsKey($relative) -or');
    Script.Add('      $entry.sha256 -notmatch ''^[a-f0-9]{64}$'' -or $entry.size -lt 0) { throw ''Invalid packaged file path or hash.'' }');
    Script.Add('  foreach ($part in $relative.Split(''/'')) {');
    Script.Add('    if ($part.EndsWith(''.'') -or $part.EndsWith('' '') -or');
    Script.Add('        $part -match ''^(?i:CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\.|$)'') { throw ''Invalid packaged Windows path.'' }');
    Script.Add('  }');
    Script.Add('  $files[$relative] = $true');
    Script.Add('  $parent = Split-Path -Parent $relative');
    Script.Add('  while ($parent) { $directories[$parent.Replace(''\'', ''/'')] = $true; $parent = Split-Path -Parent $parent }');
    Script.Add('}');
    Script.Add('$pending = New-Object ''System.Collections.Generic.Stack[string]''');
    Script.Add('$pending.Push($Root)');
    Script.Add('while ($pending.Count -gt 0) {');
    Script.Add('  foreach ($item in (Get-ChildItem -LiteralPath $pending.Pop() -Force)) {');
    Script.Add('    if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Repair tree contains a link: $($item.FullName)" }');
    Script.Add('    $relative = $item.FullName.Substring($Root.Length + 1).Replace(''\'', ''/'')');
    Script.Add('    if ($item.PSIsContainer) {');
    Script.Add('      if (-not $directories.ContainsKey($relative)) { throw "Unknown repair directory: $relative" }');
    Script.Add('      $pending.Push($item.FullName)');
    Script.Add('    } elseif (-not $files.ContainsKey($relative)) { throw "Unknown repair file: $relative" }');
    Script.Add('  }');
    Script.Add('}');
    Script.Add('foreach ($name in @(''unins000.exe'', ''unins000.dat'')) {');
    Script.Add('  if (-not (Test-Path -LiteralPath (Join-Path $Root $name) -PathType Leaf)) { throw ''The existing Inno uninstall entry is incomplete.'' }');
    Script.Add('}');
    Script.Add('$manifestPath = Join-Path $Root ''manifest.json''');
    Script.Add('$installed = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json');
    Script.Add('Assert-Manifest $installed');
    Script.Add('if ($VerifyPayload) {');
    Script.Add('  if ((Get-FileHash -LiteralPath $manifestPath -Algorithm SHA256).Hash -ne');
    Script.Add('      (Get-FileHash -LiteralPath (Join-Path $PSScriptRoot ''XMJD-repair-manifest.json'') -Algorithm SHA256).Hash) { throw ''Restored manifest differs from package.'' }');
    Script.Add('  foreach ($entry in $reference.files) {');
    Script.Add('    $path = Join-Path $Root $entry.path');
    Script.Add('    $item = Get-Item -LiteralPath $path -Force');
    Script.Add('    if ($item.PSIsContainer -or $item.Length -ne $entry.size -or');
    Script.Add('        (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() -ne $entry.sha256) { throw "Restored payload differs: $($entry.path)" }');
    Script.Add('  }');
    Script.Add('}');
    if not SaveStringToFile(ScriptPath, Script.Text, False) then
      RaiseException('Could not create the repair preflight script.');
  finally
    Script.Free;
  end;
  Parameters := '-NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "' +
    ScriptPath + '" -Root "' + FixedInstallDirectory + '" -Version "{#AppVersion}"';
  if VerifyPayload then
    Parameters := Parameters + ' -VerifyPayload';
  ResultCode := -1;
  Result := ExecAndLogOutput(ExpandConstant('{sys}\WindowsPowerShell\v1.0\powershell.exe'),
    Parameters, ExpandConstant('{tmp}'), SW_HIDE, ewWaitUntilTerminated, ResultCode, nil);
  if Result then
    Result := ResultCode = 0;
  Log(Format('Repair payload check exit code: %d', [ResultCode]));
end;

function InitializeSetup: Boolean;
var
  Version: TWindowsVersion;
  Index: Integer;
begin
  for Index := 1 to ParamCount do
    if CompareText(ParamStr(Index), '/REPAIR') = 0 then
      RepairMode := True;
  GetWindowsVersionEx(Version);
  Result := (Version.Major = 10) and (Version.Build >= 22000);
  if not Result then
    SuppressibleMsgBox('XMJD requires an x64 Windows build 22000 or newer (Windows 11). ARM64 is not supported.',
      mbCriticalError, MB_OK, IDOK);
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  InstallDirectory, PreviousVersion: String;
begin
  Result := '';
  InstallDirectory := FixedInstallDirectory;
  if CompareText(ExpandFileName(ExpandConstant('{app}')), InstallDirectory) <> 0 then
    Result := 'XMJD must be installed in ' + InstallDirectory + '. Remove any /DIR override and retry.'
  else if HasReparsePoint(ExpandConstant('{autopf}')) or
    HasReparsePoint(ExpandConstant('{autopf}\XMJD')) then
    Result := 'The XMJD installation path must not contain a symbolic link or junction.'
  else if RepairMode then
  begin
    if not DirExists(InstallDirectory) or not RepairOwnershipMatches then
      Result := 'Repair requires the same-version Inno installation in ' + InstallDirectory +
        ' with matching uninstall, COM and Host ownership. Other installations cannot be repaired by this package.'
    else
    begin
      try
        if not CheckRepairPayload(False) then
          Result := 'Repair preflight rejected the installation. Its same-version manifest must exist, and the installation tree must contain only package files and no links. See the setup log.';
      except
        Result := 'Repair preflight failed: ' + GetExceptionMessage;
      end;
    end;
  end
  else if DirExists(InstallDirectory) or FileExists(InstallDirectory) then
    Result := 'This version directory already exists: ' + InstallDirectory +
      '. To restore this same-version installation, run this installer with /REPAIR. Otherwise uninstall it first, then sign out or restart if files are still in use.'
  else if RegValueExists(HKLM64, TipClassKey, '') or
    RegValueExists(HKLM32, TipClassKey, '') or
    RegValueExists(HKLM64, 'Software\XMJD', 'HostExecutablePath') or
    RegValueExists(HKLM64, 'Software\XMJD', 'RegistrationComplete') or
    RegValueExists(HKLM64, StartupKey, 'XMJDHost') then
  begin
    if RegQueryStringValue(HKLM64, UninstallKey, 'DisplayVersion', PreviousVersion) then
      Result := 'XMJD ' + PreviousVersion + ' is already installed. Open Settings > Apps > Installed apps, ' +
        'uninstall XMJD, then run this installer again. Your user data will be preserved.'
    else
      Result := 'An XMJD installation or partial registration already exists. Uninstall the previous XMJD version first; ' +
        'your user data will be preserved. If it was installed manually, run that installation''s scripts\unregister.ps1 ' +
        'as administrator before retrying.';
  end;
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  ResultCode: Integer;
  RollbackParameters: String;
begin
  if CurStep <> ssPostInstall then
    Exit;

  { 修复先校验恢复后的文件，再用可信脚本清理本实例的部分注册。失败不得删除原安装。 }
  if RepairMode then
  begin
    InstallFailureCode := 42;
    InstallFailureMessage := 'XMJD repair failed. All program files and the uninstaller were retained. ' +
      'See the setup log, resolve the error and retry this same-version installer with /REPAIR.';
    try
      if not RepairOwnershipMatches or not CheckRepairPayload(True) then
      begin
        Log(InstallFailureMessage);
        Exit;
      end;
      if not RunPackageScript('unregister.ps1', 'InstallDirectory', ExpandConstant('{app}')) then
      begin
        Log(InstallFailureMessage);
        Exit;
      end;
      if not RunPackageScript('register.ps1', 'InstallDirectory', ExpandConstant('{app}')) then
      begin
        Log(InstallFailureMessage);
        Exit;
      end;
      InstallFailureCode := 0;
      InstallFailureMessage := '';
    except
      Log(InstallFailureMessage + ' ' + GetExceptionMessage);
    end;
    Exit;
  end;

  { 安装器只注册；Host 由普通桌面会话在激活输入法或登录时启动。 }
  if RunPackageScript('register.ps1', 'InstallDirectory', ExpandConstant('{app}')) then
    Exit;

  InstallFailureCode := 40;
  InstallFailureMessage := 'XMJD registration failed. See the setup log for details.';
  Log(InstallFailureMessage);
  if not RunPackageScript('unregister.ps1', 'InstallDirectory', ExpandConstant('{app}')) then
  begin
    InstallFailureCode := 41;
    InstallFailureMessage := InstallFailureMessage + #13#10#13#10 +
      'Cleanup could not unregister both input-method architectures. All program files and the uninstaller were retained. ' +
      'Retry uninstalling XMJD as administrator. Do not delete the installation directory manually.';
    Log(InstallFailureMessage);
    Exit;
  end;

  { ssPostInstall 已越过 Inno 自动回滚边界，必须显式调用已生成的卸载器。 }
  RollbackParameters := '/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /LOG="' +
    ExpandConstant('{tmp}\XMJD-rollback.log') + '"';
  if Exec(ExpandConstant('{uninstallexe}'), RollbackParameters,
    ExpandConstant('{app}'), SW_HIDE, ewWaitUntilTerminated, ResultCode) then
  begin
    Log(Format('Registration rollback uninstaller exit code: %d', [ResultCode]));
    if ResultCode = 0 then
      InstallFailureMessage := InstallFailureMessage + #13#10#13#10 +
        'Registration and installed program files were rolled back. Resolve the error and run Setup again.'
    else
      InstallFailureMessage := InstallFailureMessage + #13#10#13#10 +
        'Registration was undone, but program-file cleanup failed. Retry uninstalling XMJD before running Setup again.';
  end
  else
  begin
    Log(Format('Could not start the rollback uninstaller: %d', [ResultCode]));
    InstallFailureMessage := InstallFailureMessage + #13#10#13#10 +
      'Registration was undone, but the uninstaller could not be started. Uninstall XMJD before running Setup again.';
  end;
  Log(InstallFailureMessage);
end;

procedure CurPageChanged(CurPageID: Integer);
begin
  if (CurPageID = wpFinished) and (InstallFailureCode <> 0) then
  begin
    WizardForm.FinishedHeadingLabel.Caption := 'XMJD installation failed';
    WizardForm.FinishedLabel.Caption := InstallFailureMessage;
  end;
end;

function GetCustomSetupExitCode: Integer;
begin
  Result := InstallFailureCode;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usUninstall then
  begin
    if not RunPackageScript('unregister.ps1', 'InstallDirectory', ExpandConstant('{app}')) then
      { 此事件的异常发生于 Inno 删除文件前，必须保留 DLL、脚本和卸载入口供重试。 }
      RaiseException('XMJD unregistration failed. Uninstall was stopped before deleting program files. ' +
        'Keep this installation intact and retry uninstalling as administrator. See the uninstall log for details.');
  end;
end;
