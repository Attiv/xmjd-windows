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
            if (Test-Path -LiteralPath $file) { throw "故障注入的文件意外存在：$MissingPath" }
            continue
        }
        if (-not (Test-Path -LiteralPath $file -PathType Leaf) -or
            (Get-Item -LiteralPath $file).Length -ne $entry.size -or
            (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant() -ne $entry.sha256) {
            throw "安装文件与 manifest 不一致：$($entry.path)"
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
$repeat = Start-Process -FilePath $resolvedInstaller -ArgumentList ('/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /SP- /LOG="' + (Join-Path $logRoot 'repeat-install.log') + '"') -Wait -PassThru
if ($repeat.ExitCode -eq 0) { throw 'Installer unexpectedly accepted an existing version directory.' }
Assert-Registration $true
$installedManifest = Get-Content -LiteralPath (Join-Path $installRoot 'manifest.json') -Raw | ConvertFrom-Json
Assert-InstalledPayload
$manifestHash = (Get-FileHash -LiteralPath (Join-Path $installRoot 'manifest.json') -Algorithm SHA256).Hash
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
    # 所有权不匹配时 /REPAIR 也必须在写入和注销前拒绝。
    $blockedRepair = Start-Process -FilePath $resolvedInstaller -ArgumentList ('/REPAIR /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /SP- /LOG="' + (Join-Path $logRoot 'blocked-repair.log') + '"') -Wait -PassThru
    if ($blockedRepair.ExitCode -eq 0) { throw '修复意外接受了不匹配的 Host 所有权。' }
    Assert-InstalledPayload
    if ($metadata.GetValue('HostExecutablePath') -ne (Join-Path $installRoot 'ownership-mismatch.exe')) {
        throw '被拒绝的修复修改了 Host 所有权。'
    }
} finally {
    $metadata.SetValue('HostExecutablePath', (Join-Path $installRoot 'XMJDHost.exe'))
    $metadata.Dispose()
    $base.Dispose()
}
Assert-Registration $true
# 仅删除一次性 runner 的 ProgramFiles 测试安装中的 x86 DLL。
# 缺失文件应阻止真实卸载；修复必须从安装包恢复，再重建双架构注册。
$missingPath = 'Tip/x86/XMJDTip.dll'
$missingFile = Join-Path $installRoot $missingPath
Remove-Item -LiteralPath $missingFile -Force
$missingUninstall = Start-Process -FilePath $uninstaller -ArgumentList ('/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /LOG="' + (Join-Path $logRoot 'missing-dll-uninstall.log') + '"') -Wait -PassThru
if ($missingUninstall.ExitCode -eq 0) { throw '缺失 x86 DLL 时卸载未报告失败。' }
Assert-InstalledPayload $missingPath
if (-not (Test-Path -LiteralPath $uninstaller) -or
    -not (Test-Path -LiteralPath (Join-Path $installRoot 'unins000.dat'))) {
    throw '卸载失败删除了修复需要的 Inno 卸载入口。'
}
Assert-Registration $true
# 模拟本实例的部分注册，确保修复确实先注销；直接 register 会拒绝此状态。
$base = [Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::LocalMachine, [Microsoft.Win32.RegistryView]::Registry64)
$metadata = $base.OpenSubKey('Software\XMJD', $true)
try { $metadata.DeleteValue('RegistrationComplete', $true) } finally {
    $metadata.Dispose()
    $base.Dispose()
}
Invoke-CheckedProcess $resolvedInstaller ('/REPAIR /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /SP- /LOG="' + (Join-Path $logRoot 'repair.log') + '"')
Assert-InstalledPayload
if ((Get-FileHash -LiteralPath (Join-Path $installRoot 'manifest.json') -Algorithm SHA256).Hash -ne $manifestHash) {
    throw '修复后的 manifest 与安装包不一致。'
}
Assert-Registration $true
if (-not (Test-Path -LiteralPath $sentinel) -or [IO.File]::ReadAllText($sentinel) -ne $sentinelText) {
    throw '修复未保留用户数据。'
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
[IO.File]::WriteAllText((Join-Path $logRoot 'result.txt'), "PASS: silent install; x64/x86 COM; Host metadata; repeat install rejected; repeat registration; mismatched ownership blocked uninstall and repair; missing x86 DLL blocked uninstall and retained remaining payload; /REPAIR restored manifest and dual-architecture registration; uninstall; repeat unregister; repair and uninstall preserved user data.`r`n")
Write-Output 'Installer integration checks passed.'
