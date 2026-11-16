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
Source: "{#PackageDirectory}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs uninsrestartdelete

[Messages]
FinishedLabel=XMJD has been installed and registered for x64 and x86 applications.%n%nSelect XMJD manually in Windows Settings. The Host starts in your normal desktop session when XMJD is activated or at your next sign-in. Your default input method was not changed.
ConfirmUninstall=Remove XMJD and its x64/x86 input-method registration?%n%nYour local XMJD user data will be preserved. Applications using the input method will not be closed. A restart may be required to remove loaded program files.

[Code]
const
  TipClassKey = 'Software\Classes\CLSID\{5F8F59F4-E5A8-4944-960B-8A8AD3BE8857}\InprocServer32';
  StartupKey = 'Software\Microsoft\Windows\CurrentVersion\Run';
  FileAttributeReparsePoint = $400;
  InvalidFileAttributes = $FFFFFFFF;

var
  InstallFailureCode: Integer;
  InstallFailureMessage: String;

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

function InitializeSetup: Boolean;
var
  Version: TWindowsVersion;
begin
  GetWindowsVersionEx(Version);
  Result := (Version.Major = 10) and (Version.Build >= 22000);
  if not Result then
    SuppressibleMsgBox('XMJD requires an x64 Windows build 22000 or newer (Windows 11). ARM64 is not supported.',
      mbCriticalError, MB_OK, IDOK);
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  InstallDirectory: String;
begin
  Result := '';
  InstallDirectory := FixedInstallDirectory;
  if CompareText(ExpandFileName(ExpandConstant('{app}')), InstallDirectory) <> 0 then
    Result := 'XMJD must be installed in ' + InstallDirectory + '. Remove any /DIR override and retry.'
  else if HasReparsePoint(ExpandConstant('{autopf}')) or
    HasReparsePoint(ExpandConstant('{autopf}\XMJD')) then
    Result := 'The XMJD installation path must not contain a symbolic link or junction.'
  else if DirExists(InstallDirectory) or FileExists(InstallDirectory) then
    Result := 'This version directory already exists: ' + InstallDirectory +
      '. Uninstall the existing installation first, then sign out or restart if files are still in use.'
  else if RegValueExists(HKLM64, TipClassKey, '') or
    RegValueExists(HKLM32, TipClassKey, '') or
    RegValueExists(HKLM64, 'Software\XMJD', 'HostExecutablePath') or
    RegValueExists(HKLM64, 'Software\XMJD', 'RegistrationComplete') or
    RegValueExists(HKLM64, StartupKey, 'XMJDHost') then
    Result := 'An XMJD installation or partial registration already exists. Uninstall it first. ' +
      'If it was installed manually, run that installation''s scripts\unregister.ps1 as administrator before retrying.';
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  ResultCode: Integer;
  RollbackParameters: String;
begin
  if CurStep <> ssPostInstall then
    Exit;

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
