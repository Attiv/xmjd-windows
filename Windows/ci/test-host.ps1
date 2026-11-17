[CmdletBinding()]
param([Parameter(Mandatory = $true)][string] $HostPath)

$ErrorActionPreference = 'Stop'
if ($env:GITHUB_ACTIONS -ne 'true' -or -not [Environment]::Is64BitProcess) {
    throw 'This integration test is restricted to disposable GitHub Windows runners.'
}
$logRoot = Join-Path (Split-Path -Parent $PSScriptRoot) 'out/installer-tests'
$hostFile = (Resolve-Path -LiteralPath $HostPath).Path
$testUser = 'XMJDCI'
$password = ConvertTo-SecureString ('Xmjd!' + [guid]::NewGuid().ToString('N') + '7a') -AsPlainText -Force
New-LocalUser -Name $testUser -Password $password -Description 'Disposable XMJD CI standard user' -AccountNeverExpires | Out-Null
$credential = [PSCredential]::new("$env:COMPUTERNAME\$testUser", $password)
$childPath = Join-Path $logRoot 'ordinary-user-host.ps1'
$childCode = @'
param([string] $HostPath, [string] $ResultPath)
$ErrorActionPreference = 'Stop'
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
if ($principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'Test account is elevated.' }
if ([Diagnostics.Process]::GetCurrentProcess().SessionId -eq 0) { throw 'Test requires an interactive nonzero Windows session.' }
# Host 是 GUI 子系统，PowerShell 的 & 不会等待它，必须显式等待并重定向输出。
$deployOut = Join-Path (Split-Path -Parent $ResultPath) 'host-deploy.out'
$deployProcess = Start-Process -FilePath $HostPath -ArgumentList '--deploy' -Wait -PassThru -NoNewWindow -RedirectStandardOutput $deployOut -RedirectStandardError ($deployOut + '.err')
$deploy = (Get-Content -LiteralPath $deployOut -Raw -ErrorAction SilentlyContinue)
if ($deployProcess.ExitCode -ne 0 -or $deploy -notmatch 'deploy-ok') { throw "Host deployment failed: $deploy" }
$hostProcess = Start-Process -FilePath $HostPath -PassThru -WindowStyle Hidden
try {
    # Initialization has already completed. Waiting on the process with a bounded
    # timeout allows startup without a retry/sleep loop.
    if ($hostProcess.WaitForExit(2000)) { throw "Host exited early: $($hostProcess.ExitCode)" }
    $healthOut = Join-Path (Split-Path -Parent $ResultPath) 'host-health.out'
    $healthProcess = Start-Process -FilePath $HostPath -ArgumentList '--health' -Wait -PassThru -NoNewWindow -RedirectStandardOutput $healthOut -RedirectStandardError ($healthOut + '.err')
    $health = (Get-Content -LiteralPath $healthOut -Raw -ErrorAction SilentlyContinue)
    if ($healthProcess.ExitCode -ne 0 -or $health.Trim() -ne 'ok') { throw "Host health failed: $health" }
    [IO.File]::WriteAllText($ResultPath, "PASS: ordinary non-admin interactive user; real deployment; Host startup; authenticated pipe health.`r`n")
} finally {
    # This is the test's own disposable Host, never an editor or unrelated app.
    if (-not $hostProcess.HasExited) { Stop-Process -Id $hostProcess.Id -Force }
}
'@
[IO.File]::WriteAllText($childPath, $childCode, [Text.UTF8Encoding]::new($true))
& icacls.exe $logRoot /grant "${testUser}:(OI)(CI)M" | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'Could not grant CI account test-log access.' }
$result = Join-Path $logRoot 'ordinary-host-result.txt'
$powerShell = Join-Path $env:WINDIR 'System32/WindowsPowerShell/v1.0/powershell.exe'
$arguments = '-NoProfile -NonInteractive -ExecutionPolicy Bypass -File "' + $childPath + '" -HostPath "' + $hostFile + '" -ResultPath "' + $result + '"'
$process = Start-Process -FilePath $powerShell -ArgumentList $arguments -Credential $credential -LoadUserProfile -WorkingDirectory $logRoot -Wait -PassThru -RedirectStandardOutput (Join-Path $logRoot 'ordinary-host.stdout.log') -RedirectStandardError (Join-Path $logRoot 'ordinary-host.stderr.log')
if ($process.ExitCode -ne 0 -or -not (Test-Path -LiteralPath $result)) {
    Get-Content -LiteralPath (Join-Path $logRoot 'ordinary-host.stderr.log') -ErrorAction SilentlyContinue
    throw "Ordinary-user Host integration failed: $($process.ExitCode)"
}
Get-Content -LiteralPath $result
