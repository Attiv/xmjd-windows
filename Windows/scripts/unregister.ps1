[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $InstallDirectory
)

$ErrorActionPreference = 'Stop'
if (-not [Environment]::Is64BitProcess) { throw 'Run unregistration from a 64-bit PowerShell process.' }
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'TSF system unregistration requires an administrator PowerShell.'
}
$root = (Resolve-Path -LiteralPath $InstallDirectory).Path
$hostPath = Join-Path $root 'XMJDHost.exe'
$clsidPath = 'Software\Classes\CLSID\{5F8F59F4-E5A8-4944-960B-8A8AD3BE8857}\InprocServer32'
$architectures = @('x64', 'x86')
$registrars = @{
    x64 = Join-Path $env:WINDIR 'System32/regsvr32.exe'
    x86 = Join-Path $env:WINDIR 'SysWOW64/regsvr32.exe'
}
$registered = [System.Collections.Generic.List[string]]::new()
$anyClass = $false
# Complete every ownership/file check before changing either architecture.
foreach ($arch in $architectures) {
    $view = if ($arch -eq 'x64') { [Microsoft.Win32.RegistryView]::Registry64 } else { [Microsoft.Win32.RegistryView]::Registry32 }
    $base = [Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::LocalMachine, $view)
    $classKey = $null
    try {
        $classKey = $base.OpenSubKey($clsidPath, $false)
        $actual = if ($classKey) { $classKey.GetValue('', $null) } else { $null }
        if ($null -ne $actual) { $anyClass = $true }
        $expected = Join-Path $root "Tip/$arch/XMJDTip.dll"
        if ($null -ne $actual -and $actual -ne $expected) {
            throw "A different $arch XMJD TIP is registered: $actual. No registration changes made."
        }
        # Also invoke for absent COM keys: a failed registration may have left TSF metadata.
        foreach ($required in @($expected, $registrars[$arch])) {
            if (-not (Test-Path -LiteralPath $required -PathType Leaf)) { throw "No registration changes made. Missing file: $required" }
        }
        $registered.Add($arch)
    } finally {
        if ($classKey) { $classKey.Dispose() }
        $base.Dispose()
    }
}
$baseKey = [Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::LocalMachine, [Microsoft.Win32.RegistryView]::Registry64)
$key = $null
$runKey = $null
try {
    $key = $baseKey.OpenSubKey('Software\XMJD', $true)
    $registeredHostPath = if ($key) { $key.GetValue('HostExecutablePath', $null) } else { $null }
    if ($null -ne $registeredHostPath -and $registeredHostPath -ne $hostPath) {
        throw "A different XMJD Host is registered: $registeredHostPath. No registration changes made."
    }
    $runKey = $baseKey.OpenSubKey('Software\Microsoft\Windows\CurrentVersion\Run', $true)
    $expectedRunCommand = '"' + $hostPath + '"'
    $actualRun = if ($runKey) { $runKey.GetValue('XMJDHost', $null) } else { $null }
    if ($null -ne $actualRun -and $actualRun -ne $expectedRunCommand) {
        throw 'A different XMJD startup command exists. No registration changes made.'
    }
    if (-not $anyClass -and $null -eq $registeredHostPath -and $null -eq $actualRun) {
        Write-Output 'This installation is already unregistered; no changes made.'
        return
    }
    $failures = [System.Collections.Generic.List[string]]::new()
    foreach ($arch in $registered) {
        $tipPath = Join-Path $root "Tip/$arch/XMJDTip.dll"
        try {
            $process = Start-Process -FilePath $registrars[$arch] -ArgumentList "/s /u `"$tipPath`"" -Wait -PassThru
            if ($process.ExitCode -ne 0) { throw "exit code $($process.ExitCode)" }
        } catch { $failures.Add("$arch TIP: $_") }
    }
    if ($failures.Count -gt 0) {
        if ($key) { $key.DeleteValue('RegistrationComplete', $false) }
        throw "Unregistration is incomplete; ownership metadata was retained for retry. $($failures -join '; ')"
    }
    if ($key -and $key.GetValue('HostExecutablePath', $null) -eq $hostPath) {
        $key.DeleteValue('HostExecutablePath', $false)
    }
    if ($key -and $key.GetValue('RegistrationComplete', $null) -eq $hostPath) {
        $key.DeleteValue('RegistrationComplete', $false)
    }
    if ($runKey -and $runKey.GetValue('XMJDHost', $null) -eq $expectedRunCommand) {
        $runKey.DeleteValue('XMJDHost', $false)
    }
} finally {
    if ($runKey) { $runKey.Dispose() }
    if ($key) { $key.Dispose() }
    $baseKey.Dispose()
}
Write-Output 'XMJD x64/x86 TIP unregistration completed. Log off before removing loaded program files.'
Write-Output 'User data under %LOCALAPPDATA%\XMJD was preserved.'
