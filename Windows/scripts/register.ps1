[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $InstallDirectory
)

$ErrorActionPreference = 'Stop'
if (-not [Environment]::Is64BitProcess) { throw 'Run registration from a 64-bit PowerShell process.' }
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'TSF system registration requires an administrator PowerShell. Actual input and Host execution must use a normal desktop session.'
}
$root = (Resolve-Path -LiteralPath $InstallDirectory).Path
$hostPath = Join-Path $root 'XMJDHost.exe'
$clsidPath = 'Software\Classes\CLSID\{5F8F59F4-E5A8-4944-960B-8A8AD3BE8857}\InprocServer32'
$architectures = @('x64', 'x86')
$registrars = @{
    x64 = Join-Path $env:WINDIR 'System32/regsvr32.exe'
    x86 = Join-Path $env:WINDIR 'SysWOW64/regsvr32.exe'
}
$existing = @{}
foreach ($arch in $architectures) {
    $tipPath = Join-Path $root "Tip/$arch/XMJDTip.dll"
    foreach ($required in @($hostPath, $tipPath, $registrars[$arch])) {
        if (-not (Test-Path -LiteralPath $required -PathType Leaf)) { throw "No registration performed. Missing file: $required" }
    }
    $view = if ($arch -eq 'x64') { [Microsoft.Win32.RegistryView]::Registry64 } else { [Microsoft.Win32.RegistryView]::Registry32 }
    $base = [Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::LocalMachine, $view)
    $classKey = $null
    try {
        $classKey = $base.OpenSubKey($clsidPath, $false)
        $existing[$arch] = if ($classKey) { $classKey.GetValue('', $null) } else { $null }
        if ($null -ne $existing[$arch] -and $existing[$arch] -ne $tipPath) {
            throw "A different $arch XMJD TIP is registered: $($existing[$arch]). Unregister that installation first."
        }
    } finally {
        if ($classKey) { $classKey.Dispose() }
        $base.Dispose()
    }
}

$baseKey = [Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::LocalMachine, [Microsoft.Win32.RegistryView]::Registry64)
$key = $null
$runKey = $null
$runCommand = '"' + $hostPath + '"'
$runKeyPath = 'Software\Microsoft\Windows\CurrentVersion\Run'
try {
    $key = $baseKey.CreateSubKey('Software\XMJD')
    $previousPath = $key.GetValue('HostExecutablePath', $null)
    $previousComplete = $key.GetValue('RegistrationComplete', $null)
    if ($null -ne $previousPath -and $previousPath -ne $hostPath) {
        throw "A different XMJD installation is registered: $previousPath. Unregister it first."
    }
    $runKey = $baseKey.CreateSubKey($runKeyPath)
    $previousRunValue = $runKey.GetValue('XMJDHost', $null)
    if ($null -ne $previousRunValue -and $previousRunValue -ne $runCommand) {
        throw "A different XMJD Host startup command is registered: $previousRunValue. Unregister it first."
    }
    if ($null -ne $existing.x64 -or $null -ne $existing.x86) {
        if ($existing.x64 -and $existing.x86 -and $previousPath -eq $hostPath -and
            $previousComplete -eq $hostPath -and $previousRunValue -eq $runCommand) {
            Write-Output 'This exact dual-architecture installation is already registered; no changes made.'
            return
        }
        throw 'A partial XMJD registration already exists. Run unregister.ps1 for this installation, then retry registration.'
    }

    $attempted = [System.Collections.Generic.List[string]]::new()
    try {
        $key.SetValue('HostExecutablePath', $hostPath, [Microsoft.Win32.RegistryValueKind]::String)
        foreach ($arch in $architectures) {
            $tipPath = Join-Path $root "Tip/$arch/XMJDTip.dll"
            # Record before invocation so a partially failed DllRegisterServer is also undone.
            $attempted.Add($arch)
            $process = Start-Process -FilePath $registrars[$arch] -ArgumentList "/s `"$tipPath`"" -Wait -PassThru
            if ($process.ExitCode -ne 0) { throw "$arch XMJD TIP registration failed with exit code $($process.ExitCode)." }
        }
        $runKey.SetValue('XMJDHost', $runCommand, [Microsoft.Win32.RegistryValueKind]::String)
        $key.SetValue('RegistrationComplete', $hostPath, [Microsoft.Win32.RegistryValueKind]::String)
    } catch {
        $registrationError = $_
        $rollbackFailed = $false
        for ($index = $attempted.Count - 1; $index -ge 0; --$index) {
            $arch = $attempted[$index]
            $tipPath = Join-Path $root "Tip/$arch/XMJDTip.dll"
            try {
                $process = Start-Process -FilePath $registrars[$arch] -ArgumentList "/s /u `"$tipPath`"" -Wait -PassThru
                if ($process.ExitCode -ne 0) { throw "exit code $($process.ExitCode)" }
            } catch {
                $rollbackFailed = $true
                Write-Warning "Rollback failed for $arch TIP: $_"
            }
        }
        if ($runKey.GetValue('XMJDHost', $null) -eq $runCommand) {
            if ($null -eq $previousRunValue) { $runKey.DeleteValue('XMJDHost', $false) }
            else { $runKey.SetValue('XMJDHost', $previousRunValue, [Microsoft.Win32.RegistryValueKind]::String) }
        }
        $key.DeleteValue('RegistrationComplete', $false)
        if (-not $rollbackFailed) {
            if ($null -eq $previousPath) { $key.DeleteValue('HostExecutablePath', $false) }
            else { $key.SetValue('HostExecutablePath', $previousPath, [Microsoft.Win32.RegistryValueKind]::String) }
        } else {
            Write-Warning 'Partial registration remains. Run unregister.ps1 with this same InstallDirectory before retrying.'
        }
        throw $registrationError
    }
} finally {
    if ($runKey) { $runKey.Dispose() }
    if ($key) { $key.Dispose() }
    $baseKey.Dispose()
}

# 确保安装目录显式允许普通 AppContainer 读取和执行；父目录的默认 ACL
# 会随系统配置变化。此权限不替代 DLL 注册的 IMMERSIVESUPPORT 兼容类别。
& "$env:SystemRoot\System32\icacls.exe" $root '/grant' '*S-1-15-2-1:(OI)(CI)(RX)' '/T' '/Q' | Out-Null
if ($LASTEXITCODE -ne 0) {
    Write-Warning 'Could not grant packaged-app read access; the IME will not work inside SearchHost or Store apps.'
}

Write-Output 'XMJD x64/x86 TIP registration completed. Select XMJD manually in Windows Settings.'
Write-Output 'The Host starts at next logon, or asynchronously when the TIP activates. The default input method was not changed.'
