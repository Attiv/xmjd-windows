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
$repeat = Start-Process -FilePath $resolvedInstaller -ArgumentList ('/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /SP- /LOG="' + (Join-Path $logRoot 'repeat-install.log') + '"') -Wait -PassThru
if ($repeat.ExitCode -eq 0) { throw 'Installer unexpectedly accepted an existing version directory.' }
Assert-Registration $true
$installedManifest = Get-Content -LiteralPath (Join-Path $installRoot 'manifest.json') -Raw | ConvertFrom-Json
foreach ($entry in $installedManifest.files) {
    $file = Join-Path $installRoot $entry.path
    if (-not (Test-Path -LiteralPath $file -PathType Leaf) -or
        (Get-Item -LiteralPath $file).Length -ne $entry.size -or
        (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant() -ne $entry.sha256) {
        throw "Installed payload differs from manifest: $($entry.path)"
    }
}
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
    foreach ($entry in $installedManifest.files) {
        $file = Join-Path $installRoot $entry.path
        if (-not (Test-Path -LiteralPath $file -PathType Leaf) -or
            (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant() -ne $entry.sha256) {
            throw "Failed unregistration removed or changed payload: $($entry.path)"
        }
    }
    if (-not (Test-Path -LiteralPath $uninstaller)) { throw 'Failed uninstall removed its retry entry point.' }
} finally {
    $metadata.SetValue('HostExecutablePath', (Join-Path $installRoot 'XMJDHost.exe'))
    $metadata.Dispose()
    $base.Dispose()
}
Assert-Registration $true
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
[IO.File]::WriteAllText((Join-Path $logRoot 'result.txt'), "PASS: silent install; x64/x86 COM; Host metadata; repeat install rejected; repeat registration; failed unregister retained all payload and retry entry; uninstall; repeat unregister; user data preserved.`r`n")
Write-Output 'Installer integration checks passed.'
