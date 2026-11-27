[CmdletBinding()]
param([Parameter(Mandatory = $true)][string] $InstallerPath)

$ErrorActionPreference = 'Stop'
if ($env:GITHUB_ACTIONS -ne 'true' -or -not [Environment]::Is64BitProcess) {
    throw 'This test changes system registrations and is restricted to disposable GitHub Windows runners.'
}
$windowsRoot = Split-Path -Parent $PSScriptRoot
$logRoot = Join-Path $windowsRoot 'out/installer-tests'
New-Item -ItemType Directory -Force -Path $logRoot | Out-Null
$parts = @{}
foreach ($line in (Get-Content -LiteralPath (Join-Path $windowsRoot 'VERSION'))) {
    if ($line -match '^(MAJOR|MINOR|PATCH|BUILD)=([0-9]+)$') { $parts[$Matches[1]] = $Matches[2] }
}
$version = '{0}.{1}.{2}+{3}' -f $parts.MAJOR, $parts.MINOR, $parts.PATCH, $parts.BUILD
$installRoot = Join-Path $env:ProgramFiles "XMJD/$version"
$clsid = 'Software\Classes\CLSID\{5F8F59F4-E5A8-4944-960B-8A8AD3BE8857}\InprocServer32'

function Invoke-CheckedProcess([string] $File, [string] $Arguments) {
    $process = Start-Process -FilePath $File -ArgumentList $Arguments -Wait -PassThru
    if ($process.ExitCode -ne 0) { throw "Process failed ($($process.ExitCode)): $File" }
}

function Assert-InstalledPayload([string] $MissingPath = '') {
    foreach ($entry in $script:installedManifest.files) {
        $file = Join-Path $script:installRoot $entry.path
        if ($entry.path -eq $MissingPath) {
            if (Test-Path -LiteralPath $file) { throw "Fault-injected file unexpectedly exists: $MissingPath" }
            continue
        }
        if (-not (Test-Path -LiteralPath $file -PathType Leaf) -or
            (Get-Item -LiteralPath $file).Length -ne $entry.size -or
            (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant() -ne $entry.sha256) {
            throw "Installed file does not match the manifest: $($entry.path)"
        }
    }
}

function Assert-Registration([bool] $Present) {
    foreach ($arch in @('x64', 'x86')) {
        $view = if ($arch -eq 'x64') { [Microsoft.Win32.RegistryView]::Registry64 } else { [Microsoft.Win32.RegistryView]::Registry32 }
        $base = [Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::LocalMachine, $view)
        $key = $null
        try {
            $key = $base.OpenSubKey($clsid)
            $actual = if ($key) { $key.GetValue('', $null) } else { $null }
            $expected = Join-Path $installRoot "Tip/$arch/XMJDTip.dll"
            if ($Present -and $actual -ne $expected) { throw "Incorrect $arch registration: $actual" }
            if (-not $Present -and $null -ne $actual) { throw "$arch registration survived uninstall: $actual" }
            $tip = $base.OpenSubKey('Software\Microsoft\CTF\TIP\{5F8F59F4-E5A8-4944-960B-8A8AD3BE8857}')
            try {
                if ($Present -and -not $tip) { throw "Missing $arch TSF profile registration." }
                if (-not $Present -and $tip) { throw "$arch TSF metadata survived uninstall." }
            } finally { if ($tip) { $tip.Dispose() } }
        } finally {
            if ($key) { $key.Dispose() }
            $base.Dispose()
        }
    }
    $base = [Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::LocalMachine, [Microsoft.Win32.RegistryView]::Registry64)
    $metadata = $null
    $startup = $null
    try {
        $metadata = $base.OpenSubKey('Software\XMJD')
        $startup = $base.OpenSubKey('Software\Microsoft\Windows\CurrentVersion\Run')
        $hostPath = Join-Path $installRoot 'XMJDHost.exe'
        foreach ($name in @('HostExecutablePath', 'RegistrationComplete')) {
            $actual = if ($metadata) { $metadata.GetValue($name, $null) } else { $null }
            if ($Present -and $actual -ne $hostPath) { throw "Incorrect $name metadata." }
            if (-not $Present -and $null -ne $actual) { throw "$name survived uninstall." }
        }
        $run = if ($startup) { $startup.GetValue('XMJDHost', $null) } else { $null }
        if ($Present -and $run -ne ('"' + $hostPath + '"')) { throw 'Incorrect Host startup command.' }
        if (-not $Present -and $null -ne $run) { throw 'Host startup command survived uninstall.' }
    } finally {
        if ($metadata) { $metadata.Dispose() }
        if ($startup) { $startup.Dispose() }
        $base.Dispose()
    }
}

$resolvedInstaller = (Resolve-Path -LiteralPath $InstallerPath).Path
if (Test-Path -LiteralPath $installRoot) { throw "Unexpected existing installation: $installRoot" }
Assert-Registration $false
$powerShell = Join-Path $env:WINDIR 'System32/WindowsPowerShell/v1.0/powershell.exe'
Invoke-CheckedProcess $resolvedInstaller ('/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /SP- /LOG="' + (Join-Path $logRoot 'install.log') + '"')
Assert-Registration $true
# Inspect the effective TSF capability bits in both registry/COM views. Merely
# finding the COM DLL and language profile misses desktop-only registrations.
foreach ($architecture in @('x64', 'x86')) {
    $registrationCheck = Join-Path $windowsRoot "out/build/$architecture/Release/XMJDTipComTests.exe"
    Invoke-CheckedProcess $registrationCheck '--check-registration'
}
$repeat = Start-Process -FilePath $resolvedInstaller -ArgumentList ('/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /SP- /LOG="' + (Join-Path $logRoot 'repeat-install.log') + '"') -Wait -PassThru
if ($repeat.ExitCode -eq 0) { throw 'Installer unexpectedly accepted an existing version directory.' }
Assert-Registration $true
$installedManifest = Get-Content -LiteralPath (Join-Path $installRoot 'manifest.json') -Raw | ConvertFrom-Json
Assert-InstalledPayload
$manifestHash = (Get-FileHash -LiteralPath (Join-Path $installRoot 'manifest.json') -Algorithm SHA256).Hash
$settingsExe = Join-Path $installRoot 'XMJDSettings.exe'
if (-not (Test-Path -LiteralPath $settingsExe -PathType Leaf)) { throw 'Missing XMJDSettings.exe.' }
$settingsCheck = Start-Process -FilePath $settingsExe -ArgumentList '--check' -Wait -PassThru
if ($settingsCheck.ExitCode -ne 0) { throw "Settings self-check failed: $($settingsCheck.ExitCode)" }
$settingsUi = Start-Process -FilePath $settingsExe -ArgumentList '--self-test-ui' -PassThru
if (-not $settingsUi.WaitForExit(15000)) {
    $settingsUi.Kill()
    throw 'Settings UI self-test timed out.'
}
if ($settingsUi.ExitCode -ne 0) { throw "Settings UI self-test failed: $($settingsUi.ExitCode)" }
$settingsSelect = Start-Process -FilePath $settingsExe -ArgumentList '--select pinyin_simp' -Wait -PassThru
if ($settingsSelect.ExitCode -ne 0) { throw "Settings scheme selection failed: $($settingsSelect.ExitCode)" }
$settingsRoot = Join-Path $env:LOCALAPPDATA 'XMJD'
if (-not (Select-String -LiteralPath (Join-Path $settingsRoot 'settings.ini') -Pattern '^schema=pinyin_simp$' -Quiet)) {
    throw 'Settings did not persist the selected scheme.'
}
$customConfig = Get-Content -LiteralPath (Join-Path $settingsRoot 'UserData/Rime/default.custom.yaml') -Raw
if ($customConfig -notmatch '(?m)^\s+- schema: pinyin_simp\s*$') {
    throw 'Settings did not write schema_list to default.custom.yaml.'
}
# Keep the test script ASCII-compatible with Windows PowerShell 5.1.
$brandName = ConvertFrom-Json '"\u661f\u732b\u952e\u9053"'
$settingsName = $brandName + (ConvertFrom-Json '"\u8bbe\u7f6e"')
$settingsShortcut = Join-Path $env:ProgramData "Microsoft/Windows/Start Menu/Programs/$brandName/$settingsName.lnk"
if (-not (Test-Path -LiteralPath $settingsShortcut -PathType Leaf)) { throw 'Missing Start Menu settings shortcut.' }

# Verify the default skin and persistence across Settings launches.
$styleListFile = Join-Path $logRoot 'styles.txt'
$styleList = Start-Process -FilePath $settingsExe -ArgumentList '--styles' -Wait -PassThru -RedirectStandardOutput $styleListFile
if ($styleList.ExitCode -ne 0) { throw "Settings style listing failed: $($styleList.ExitCode)" }
$styleText = [IO.File]::ReadAllText($styleListFile, [Text.Encoding]::UTF8)
if ($styleText -notmatch 'active=system') { throw 'The system skin is not active by default.' }
if ($styleText -notmatch 'builtin orbit') { throw 'The built-in orbit skin is missing.' }
$stylePick = Start-Process -FilePath $settingsExe -ArgumentList '--select-style orbit' -Wait -PassThru
if ($stylePick.ExitCode -ne 0) { throw "Settings style selection failed: $($stylePick.ExitCode)" }
$activeStylePath = Join-Path $settingsRoot 'CandidateStyles/active.json'
if (-not (Test-Path -LiteralPath $activeStylePath -PathType Leaf)) { throw 'Candidate style was not persisted.' }
if ((Get-Content -LiteralPath $activeStylePath -Raw) -notmatch '"id":\s*"orbit"') { throw 'active.json does not contain the selected style.' }
$styleList2 = Start-Process -FilePath $settingsExe -ArgumentList '--styles' -Wait -PassThru -RedirectStandardOutput $styleListFile
if ($styleList2.ExitCode -ne 0 -or [IO.File]::ReadAllText($styleListFile, [Text.Encoding]::UTF8) -notmatch 'active=orbit') {
    throw 'Settings did not restore the active skin.'
}
Write-Output 'PASS: settings app self-check, candidate style selection and persistence.'

# Import a minimal schema and check its files and list entry.
$schemeFixture = Join-Path $logRoot 'ci-scheme-source'
Remove-Item -LiteralPath $schemeFixture -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $schemeFixture | Out-Null
[IO.File]::WriteAllText((Join-Path $schemeFixture 'ci_test.schema.yaml'),
    "schema:`n  schema_id: ci_test`n  name: CI $(ConvertFrom-Json '"\u6d4b\u8bd5\u65b9\u6848"')`n")
[IO.File]::WriteAllText((Join-Path $schemeFixture 'default.yaml'), "schema_list:`n  - schema: ci_test`n")
$importScheme = Start-Process -FilePath $settingsExe -ArgumentList ('--import-scheme "' + $schemeFixture + '"') -Wait -PassThru -RedirectStandardOutput (Join-Path $logRoot 'import-scheme.txt')
if ($importScheme.ExitCode -ne 0) { throw "Settings scheme import failed: $($importScheme.ExitCode)" }
$importedSchema = Join-Path $settingsRoot 'UserData/Rime/imported/ci-scheme-source/ci_test.schema.yaml'
if (Test-Path -LiteralPath $importedSchema) { throw 'Settings must not create a filtered copy of the selected source.' }
if (-not (Select-String -LiteralPath (Join-Path $settingsRoot 'settings.ini') -Pattern '^scheme_source=' -Quiet)) {
    throw 'Settings did not record the external schema source.'
}
$schemaListFile = Join-Path $logRoot 'schemas.txt'
$schemaList = Start-Process -FilePath $settingsExe -ArgumentList '--schemas' -Wait -PassThru -RedirectStandardOutput $schemaListFile
if ($schemaList.ExitCode -ne 0) { throw "Settings schema listing failed: $($schemaList.ExitCode)" }
$schemaText = [IO.File]::ReadAllText($schemaListFile, [Text.Encoding]::UTF8)
if ($schemaText -notmatch 'schema ci_test ') { throw 'The imported schema is missing from the schema list.' }
if ($schemaText -notmatch '(?m)^schema ci_test .* imported\r?$') { throw 'The schema list does not mark the schema as imported.' }
if ($schemaText -match '(?m)^schema xmjd6 ') { throw 'The external source was mixed with the built-in scheme list.' }
$settingsCheckAfterImport = Start-Process -FilePath $settingsExe -ArgumentList '--check' -Wait -PassThru
if ($settingsCheckAfterImport.ExitCode -ne 0) { throw "Settings self-check after import failed: $($settingsCheckAfterImport.ExitCode)" }
$removeSchemes = Start-Process -FilePath $settingsExe -ArgumentList '--remove-schemes' -Wait -PassThru
if ($removeSchemes.ExitCode -ne 0) { throw "Removing imported schemes failed: $($removeSchemes.ExitCode)" }
if (Test-Path -LiteralPath (Join-Path $settingsRoot 'UserData/Rime/imported')) { throw 'The imported data directory survived removal.' }
if (-not (Test-Path -LiteralPath (Join-Path $schemeFixture 'ci_test.schema.yaml') -PathType Leaf)) {
    throw 'Removing the source selection modified the external scheme.'
}
Write-Output 'PASS: scheme import, listing, self-check and removal.'
Invoke-CheckedProcess $powerShell ('-NoProfile -NonInteractive -ExecutionPolicy Bypass -File "' + (Join-Path $installRoot 'scripts/register.ps1') + '" -InstallDirectory "' + $installRoot + '"')
Assert-Registration $true
& (Join-Path $PSScriptRoot 'test-host.ps1') -HostPath (Join-Path $installRoot 'XMJDHost.exe')

# A fresh runner account has no personal product data. Leave a sentinel and
# verify that the actual uninstaller preserves it and all engine test data.
$dataRoot = Join-Path $env:LOCALAPPDATA 'XMJD'
New-Item -ItemType Directory -Force -Path $dataRoot | Out-Null
$sentinel = Join-Path $dataRoot 'ci-preserve-user-data.txt'
$sentinelText = [guid]::NewGuid().ToString('N')
[IO.File]::WriteAllText($sentinel, $sentinelText)
$uninstaller = Join-Path $installRoot 'unins000.exe'
# Exercise the actual uninstaller's failure boundary with a reversible ownership
# mismatch on this disposable runner. It must stop before deleting any payload.
$base = [Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::LocalMachine, [Microsoft.Win32.RegistryView]::Registry64)
$metadata = $base.OpenSubKey('Software\XMJD', $true)
try {
    $metadata.SetValue('HostExecutablePath', (Join-Path $installRoot 'ownership-mismatch.exe'))
    $failedUninstall = Start-Process -FilePath $uninstaller -ArgumentList ('/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /LOG="' + (Join-Path $logRoot 'blocked-uninstall.log') + '"') -Wait -PassThru
    if ($failedUninstall.ExitCode -eq 0) { throw 'Uninstaller did not report failed unregistration.' }
    Assert-InstalledPayload
    if (-not (Test-Path -LiteralPath $uninstaller)) { throw 'Failed uninstall removed its retry entry point.' }
    # /REPAIR must reject mismatched ownership before writing or unregistering.
    $blockedRepair = Start-Process -FilePath $resolvedInstaller -ArgumentList ('/REPAIR /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /SP- /LOG="' + (Join-Path $logRoot 'blocked-repair.log') + '"') -Wait -PassThru
    if ($blockedRepair.ExitCode -eq 0) { throw 'Repair accepted mismatched Host ownership.' }
    Assert-InstalledPayload
    if ($metadata.GetValue('HostExecutablePath') -ne (Join-Path $installRoot 'ownership-mismatch.exe')) {
        throw 'The rejected repair changed Host ownership.'
    }
} finally {
    $metadata.SetValue('HostExecutablePath', (Join-Path $installRoot 'XMJDHost.exe'))
    $metadata.Dispose()
    $base.Dispose()
}
Assert-Registration $true
# Remove only the x86 DLL in the disposable runner install. Uninstall must fail
# with the file missing; repair must restore it and rebuild both registrations.
$missingPath = 'Tip/x86/XMJDTip.dll'
$missingFile = Join-Path $installRoot $missingPath
Remove-Item -LiteralPath $missingFile -Force
$missingUninstall = Start-Process -FilePath $uninstaller -ArgumentList ('/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /LOG="' + (Join-Path $logRoot 'missing-dll-uninstall.log') + '"') -Wait -PassThru
if ($missingUninstall.ExitCode -eq 0) { throw 'Uninstall did not fail when the x86 DLL was missing.' }
Assert-InstalledPayload $missingPath
if (-not (Test-Path -LiteralPath $uninstaller) -or
    -not (Test-Path -LiteralPath (Join-Path $installRoot 'unins000.dat'))) {
    throw 'The failed uninstall removed the Inno uninstaller needed for repair.'
}
Assert-Registration $true
# Simulate a partial registration: repair must unregister before registering.
$base = [Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::LocalMachine, [Microsoft.Win32.RegistryView]::Registry64)
$metadata = $base.OpenSubKey('Software\XMJD', $true)
try { $metadata.DeleteValue('RegistrationComplete', $true) } finally {
    $metadata.Dispose()
    $base.Dispose()
}
Invoke-CheckedProcess $resolvedInstaller ('/REPAIR /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /SP- /LOG="' + (Join-Path $logRoot 'repair.log') + '"')
Assert-InstalledPayload
if ((Get-FileHash -LiteralPath (Join-Path $installRoot 'manifest.json') -Algorithm SHA256).Hash -ne $manifestHash) {
    throw 'The repaired manifest does not match the installation package.'
}
Assert-Registration $true
if (-not (Test-Path -LiteralPath $sentinel) -or [IO.File]::ReadAllText($sentinel) -ne $sentinelText) {
    throw 'Repair did not preserve user data.'
}
Invoke-CheckedProcess $uninstaller ('/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /LOG="' + (Join-Path $logRoot 'uninstall.log') + '"')
Assert-Registration $false
if (-not (Test-Path -LiteralPath $sentinel) -or [IO.File]::ReadAllText($sentinel) -ne $sentinelText) {
    throw 'Uninstall did not preserve user data.'
}
# The second unregister call uses verified package files because the uninstaller
# has correctly removed its own program directory.
$package = Join-Path $windowsRoot "out/package/XMJD-$version-windows-x64-x86-Release"
Invoke-CheckedProcess $powerShell ('-NoProfile -NonInteractive -ExecutionPolicy Bypass -File "' + (Join-Path $package 'scripts/unregister.ps1') + '" -InstallDirectory "' + $package + '"')
Assert-Registration $false
if (Test-Path -LiteralPath (Join-Path $installRoot 'XMJDHost.exe')) { throw 'Program files survived a clean uninstall.' }
if (Test-Path -LiteralPath (Join-Path $installRoot 'XMJDSettings.exe')) { throw 'Settings program survived a clean uninstall.' }
if (Test-Path -LiteralPath $settingsShortcut) { throw 'Settings Start Menu shortcut survived uninstall.' }
[IO.File]::WriteAllText((Join-Path $logRoot 'result.txt'), "PASS: silent install; x64/x86 COM; Host metadata; settings self-check, dialog self-test and scheme persistence; candidate style selection; scheme import and removal; Start Menu shortcut; repeat install rejected; repeat registration; mismatched ownership blocked uninstall and repair; missing x86 DLL blocked uninstall and retained remaining payload; /REPAIR restored manifest and dual-architecture registration; uninstall; repeat unregister; repair and uninstall preserved user data.`r`n")
Write-Output 'Installer integration checks passed.'
