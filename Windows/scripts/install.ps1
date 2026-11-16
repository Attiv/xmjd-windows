[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string] $Configuration = 'Release',
    [string] $PackageDirectory
)

$ErrorActionPreference = 'Stop'
if (-not [Environment]::Is64BitProcess) { throw 'Run installation from 64-bit PowerShell.' }
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Open a 64-bit PowerShell as administrator for installation. Build and run the Host from a normal desktop session.'
}
$windowsRoot = Split-Path -Parent $PSScriptRoot
$versionParts = @{}
foreach ($line in (Get-Content -LiteralPath (Join-Path $windowsRoot 'VERSION'))) {
    if ($line -notmatch '^(MAJOR|MINOR|PATCH|BUILD)=([0-9]+)$') { throw 'Windows/VERSION is malformed.' }
    $part = $Matches[1]
    $value = $Matches[2]
    if ($versionParts.ContainsKey($part) -or [long] $value -gt 65535) { throw "Invalid Windows/VERSION field: $part" }
    $versionParts[$part] = $value
}
foreach ($part in @('MAJOR', 'MINOR', 'PATCH', 'BUILD')) {
    if (-not $versionParts.ContainsKey($part)) { throw "Windows/VERSION is missing $part." }
}
$version = '{0}.{1}.{2}+{3}' -f $versionParts.MAJOR, $versionParts.MINOR, $versionParts.PATCH, $versionParts.BUILD
if (-not $PackageDirectory) {
    # A script inside an unpacked delivery can install that delivery directly.
    if (Test-Path -LiteralPath (Join-Path $windowsRoot 'manifest.json') -PathType Leaf) {
        $PackageDirectory = $windowsRoot
    } else {
        $PackageDirectory = Join-Path $windowsRoot "out/package/XMJD-$version-windows-x64-x86-$Configuration"
    }
}
$packageRoot = (Resolve-Path -LiteralPath $PackageDirectory).Path
& (Join-Path $PSScriptRoot 'build.ps1') -VerifyPackage $packageRoot
$manifest = Get-Content -LiteralPath (Join-Path $packageRoot 'manifest.json') -Raw | ConvertFrom-Json
if ($manifest.version -ne $version -or $manifest.configuration -ne $Configuration) {
    throw 'Package version/configuration differs from this installation script. Use the matching delivery.'
}
$programFiles = [Environment]::GetFolderPath([Environment+SpecialFolder]::ProgramFiles)
$installRoot = Join-Path $programFiles "XMJD/$version"
if (Test-Path -LiteralPath $installRoot) { throw "Refusing to overwrite an existing installed version: $installRoot" }
$installParent = Split-Path -Parent $installRoot
New-Item -ItemType Directory -Force -Path $installParent | Out-Null
if ((Get-Item -LiteralPath $installParent -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) {
    throw 'The installation parent must not be a junction or symbolic link.'
}
$stagingRoot = Join-Path $installParent ".staging-$([guid]::NewGuid().ToString('N'))"
try {
    New-Item -ItemType Directory -Path $stagingRoot | Out-Null
    foreach ($item in (Get-ChildItem -LiteralPath $packageRoot -Force)) {
        Copy-Item -LiteralPath $item.FullName -Destination $stagingRoot -Recurse
    }
    & (Join-Path $PSScriptRoot 'build.ps1') -VerifyPackage $stagingRoot
    [IO.Directory]::Move($stagingRoot, $installRoot)
} finally {
    if (Test-Path -LiteralPath $stagingRoot) { Remove-Item -LiteralPath $stagingRoot -Recurse -Force }
}
Write-Output "Installed verified x64 Host and x64/x86 TIP package $version to $installRoot"
Write-Output 'Registration is a separate explicit administrator step. Runtime and user data always belong to the ordinary desktop user.'
