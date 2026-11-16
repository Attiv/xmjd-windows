[CmdletBinding(DefaultParameterSetName = 'Build')]
param(
    [Parameter(ParameterSetName = 'Build')]
    [ValidateSet('all', 'x64', 'x86')]
    [string] $Architecture = 'all',
    [Parameter(ParameterSetName = 'Build')]
    [ValidateSet('Debug', 'Release')]
    [string] $Configuration = 'Release',
    [Parameter(ParameterSetName = 'Build')]
    [switch] $SkipDependencyPreparation,
    [Parameter(Mandatory = $true, ParameterSetName = 'Verify')]
    [string] $VerifyPackage
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression.FileSystem
Add-Type -AssemblyName System.IO.Compression
$windowsRoot = Split-Path -Parent $PSScriptRoot
$manifestName = 'manifest.json'

function Assert-PackagePath([string] $Path) {
    if ([string]::IsNullOrWhiteSpace($Path) -or $Path -match '[\\:\x00-\x1f<>"|?*]' -or
        $Path.StartsWith('/') -or $Path -match '(^|/)(\.|\.\.|)(/|$)') { throw "Unsafe package path: $Path" }
    foreach ($part in $Path.Split('/')) {
        if ($part.EndsWith('.') -or $part.EndsWith(' ') -or $part -match '^(?i:CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\.|$)') {
            throw "Unsafe Windows package path: $Path"
        }
    }
}

function Get-PackageInventory([string] $Root) {
    $rootItem = Get-Item -LiteralPath $Root -Force
    if (-not $rootItem.PSIsContainer -or ($rootItem.Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw 'Package must be an ordinary directory.' }
    $prefix = $rootItem.FullName.TrimEnd([char[]]@('/', '\')) + [IO.Path]::DirectorySeparatorChar
    $pending = New-Object 'System.Collections.Generic.Stack[string]'
    $pending.Push($rootItem.FullName)
    $files = @()
    while ($pending.Count -gt 0) {
        foreach ($item in (Get-ChildItem -LiteralPath $pending.Pop() -Force)) {
            if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Package contains a link: $($item.FullName)" }
            if ($item.PSIsContainer) { $pending.Push($item.FullName) } else { $files += $item }
        }
    }
    foreach ($file in ($files | Sort-Object FullName)) {
        $relative = $file.FullName.Substring($prefix.Length).Replace('\', '/')
        Assert-PackagePath $relative
        if ($relative -eq $manifestName) { continue }
        [ordered]@{ path = $relative; size = $file.Length; sha256 = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant() }
    }
}

function Get-ManifestFiles([object] $Manifest) {
    if ($Manifest.formatVersion -ne 1 -or $Manifest.version -notmatch '^\d+\.\d+\.\d+\+\d+$' -or
        ($Manifest.architectures -join ',') -ne 'x64,x86' -or $Manifest.configuration -notin @('Debug', 'Release')) { throw 'Invalid delivery manifest.' }
    $expected = @{}
    foreach ($entry in $Manifest.files) {
        Assert-PackagePath $entry.path
        if ($entry.path -eq $manifestName -or $expected.ContainsKey($entry.path) -or
            $entry.sha256 -notmatch '^[a-f0-9]{64}$' -or $entry.size -lt 0) { throw "Invalid manifest entry: $($entry.path)" }
        $expected[$entry.path] = $entry
    }
    foreach ($path in @('XMJDHost.exe', 'XMJDSettings.exe', 'rime.dll', 'Tip/x64/XMJDTip.dll', 'Tip/x86/XMJDTip.dll',
        'librime.lock.json', 'xmjd-librime-lock.json', 'version-info.txt', 'VERSION',
        'Resources/Rime/xmjd6.schema.yaml', 'Resources/Rime/default.yaml', 'Resources/Rime/default.custom.yaml', 'Resources/Rime/essay.txt',
        'Resources/Rime/opencc/s2t.json', 'Resources/Rime/opencc/STCharacters.ocd2',
        'share/opencc/s2t.json', 'licenses/librime-LICENSE', 'README.md', 'IMPLEMENTATION_PLAN.md',
        'LICENSE.txt', 'THIRD_PARTY_NOTICES.txt',
        'scripts/build.ps1', 'scripts/install.ps1', 'scripts/register.ps1', 'scripts/unregister.ps1')) {
        if (-not $expected.ContainsKey($path)) { throw "Incomplete delivery manifest: $path" }
    }
    return $expected
}

function Assert-Package([string] $Path) {
    if (Test-Path -LiteralPath $Path -PathType Container) {
        $actual = @(Get-PackageInventory $Path)
        $manifest = Get-Content -LiteralPath (Join-Path $Path $manifestName) -Raw | ConvertFrom-Json
        $expected = Get-ManifestFiles $manifest
        if ($actual.Count -ne $expected.Count) { throw 'Delivery file count differs from manifest.' }
        foreach ($entry in $actual) {
            $reference = $expected[$entry.path]
            if (-not $reference -or $entry.size -ne $reference.size -or $entry.sha256 -ne $reference.sha256) { throw "Delivery content differs: $($entry.path)" }
        }
    } else {
        $zip = [IO.Compression.ZipFile]::OpenRead([IO.Path]::GetFullPath($Path))
        try {
            $entries = @{}
            foreach ($entry in $zip.Entries) {
                $relative = $entry.FullName.TrimEnd('/')
                Assert-PackagePath $relative
                if (($entry.ExternalAttributes -band 0x400) -ne 0 -or
                    (($entry.ExternalAttributes -shr 16) -band 0xf000) -eq 0xa000) { throw "ZIP link is not allowed: $relative" }
                if ($entries.ContainsKey($relative)) { throw "Duplicate ZIP entry: $relative" }
                $entries[$relative] = $entry
            }
            if (-not $entries.ContainsKey($manifestName)) { throw 'ZIP manifest is missing.' }
            $reader = New-Object IO.StreamReader($entries[$manifestName].Open())
            try { $manifest = $reader.ReadToEnd() | ConvertFrom-Json } finally { $reader.Dispose() }
            $expected = Get-ManifestFiles $manifest
            $fileEntries = @($entries.Values | Where-Object { -not $_.FullName.EndsWith('/') -and $_.FullName -ne $manifestName })
            if ($fileEntries.Count -ne $expected.Count) { throw 'ZIP file count differs from manifest.' }
            foreach ($entry in $fileEntries) {
                $reference = $expected[$entry.FullName]
                if (-not $reference -or $entry.Length -ne $reference.size) { throw "Unexpected ZIP entry: $($entry.FullName)" }
                $stream = $entry.Open()
                $hasher = [Security.Cryptography.SHA256]::Create()
                try { $hash = [BitConverter]::ToString($hasher.ComputeHash($stream)).Replace('-', '').ToLowerInvariant() }
                finally { $hasher.Dispose(); $stream.Dispose() }
                if ($hash -ne $reference.sha256) { throw "ZIP content differs: $($entry.FullName)" }
            }
        } finally { $zip.Dispose() }
    }
    Write-Output "Verified delivery manifest and all content hashes: $Path"
}

if ($PSCmdlet.ParameterSetName -eq 'Verify') {
    Assert-Package $VerifyPackage
    return
}
if ([Environment]::OSVersion.Platform -ne [PlatformID]::Win32NT -or -not [Environment]::Is64BitProcess) {
    throw 'Native builds require 64-bit PowerShell on Windows; -VerifyPackage also works on other platforms.'
}
$cmake = Get-Command 'cmake.exe' -ErrorAction SilentlyContinue
if (-not $cmake) { $cmake = Get-Command 'cmake' -ErrorAction SilentlyContinue }
if (-not $cmake) { throw 'CMake 3.24 or newer is required.' }
$cmakeVersion = (& $cmake.Source --version | Select-Object -First 1)
if ($cmakeVersion -notmatch 'cmake version ([0-9]+\.[0-9]+\.[0-9]+)') { throw 'Could not read CMake version.' }
if ([version] $Matches[1] -lt [version] '3.24.0') { throw "CMake 3.24 or newer is required; found $($Matches[1])." }
$ctestPath = Join-Path (Split-Path -Parent $cmake.Source) 'ctest.exe'
if (-not (Test-Path -LiteralPath $ctestPath -PathType Leaf)) { throw "CTest not found alongside CMake: $ctestPath" }
$versionParts = @{}
foreach ($line in (Get-Content -LiteralPath (Join-Path $windowsRoot 'VERSION'))) {
    if ($line -notmatch '^(MAJOR|MINOR|PATCH|BUILD)=([0-9]+)$') { throw 'Windows/VERSION is malformed.' }
    $part = $Matches[1]
    if ($versionParts.ContainsKey($part) -or [long] $Matches[2] -gt 65535) { throw "Invalid Windows/VERSION field: $part" }
    $versionParts[$part] = $Matches[2]
}
foreach ($part in @('MAJOR', 'MINOR', 'PATCH', 'BUILD')) {
    if (-not $versionParts.ContainsKey($part)) { throw "Windows/VERSION is missing $part." }
}
$version = '{0}.{1}.{2}+{3}' -f $versionParts.MAJOR, $versionParts.MINOR, $versionParts.PATCH, $versionParts.BUILD
$architectures = if ($Architecture -eq 'all') { @('x64', 'x86') } else { @($Architecture) }
$lock = Get-Content -LiteralPath (Join-Path $windowsRoot 'librime.lock.json') -Raw | ConvertFrom-Json
$schemePath = Join-Path $windowsRoot $lock.scheme.path
if ((Get-FileHash -LiteralPath $schemePath -Algorithm SHA256).Hash.ToLowerInvariant() -ne $lock.scheme.sha256) { throw 'Scheme archive SHA-256 differs from lock.' }
$packageParent = Join-Path $windowsRoot 'out/package'
$packageName = "XMJD-$version-windows-x64-x86-$Configuration"
$packageRoot = Join-Path $packageParent $packageName
$zipPath = "$packageRoot.zip"
if ($Architecture -eq 'all' -and ((Test-Path -LiteralPath $packageRoot) -or (Test-Path -LiteralPath $zipPath))) {
    throw "Delivery already exists; preserve it or explicitly move it before rebuilding: $packageRoot"
}
if ('x64' -in $architectures) {
    & (Join-Path $PSScriptRoot 'prepare-deps.ps1') -Architecture x64 -VerifyOnly:$SkipDependencyPreparation
}
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) { throw 'Visual Studio Installer/vswhere.exe is required.' }
$vsVersion = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationVersion
if ($LASTEXITCODE -ne 0 -or -not $vsVersion) { throw 'No Visual Studio C++ x64/x86 toolchain was found.' }
$vsMajor = ([version]($vsVersion | Select-Object -First 1)).Major
$generator = switch ($vsMajor) {
    17 { 'Visual Studio 17 2022' }
    18 { 'Visual Studio 18 2026' }
    default { throw "Unsupported Visual Studio major version: $vsMajor" }
}
Write-Output "Using $generator ($vsVersion)."
$buildDirectories = @{}
$testEvidence = @()
foreach ($arch in $architectures) {
    $buildDirectory = Join-Path $windowsRoot "out/build/$arch"
    $cmakeArchitecture = if ($arch -eq 'x64') { 'x64' } else { 'Win32' }
    $configureArguments = @('-S', $windowsRoot, '-B', $buildDirectory, '-G', $generator,
        '-A', $cmakeArchitecture, '-DBUILD_TESTING=ON')
    if ($arch -eq 'x64') { $configureArguments += '-DXMJD_RIME_ROOT=' + (Join-Path $windowsRoot ".deps/librime/$($lock.version)/x64") }
    & $cmake.Source @configureArguments
    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed: $arch" }
    & $cmake.Source --build $buildDirectory --config $Configuration --parallel
    if ($LASTEXITCODE -ne 0) { throw "CMake build failed: $arch" }
    & $ctestPath --test-dir $buildDirectory -C $Configuration --output-on-failure --no-tests=error
    if ($LASTEXITCODE -ne 0) { throw "CTest failed: $arch" }
    $testList = & $ctestPath --test-dir $buildDirectory -C $Configuration --show-only=json-v1
    if ($LASTEXITCODE -ne 0) { throw "CTest enumeration failed: $arch" }
    $tests = ($testList -join "`n") | ConvertFrom-Json
    $testEvidence += [ordered]@{ architecture = $arch; result = 'passed'; tests = @($tests.tests | ForEach-Object { $_.name }) }
    $versionHeader = Join-Path $buildDirectory 'generated/Version.h'
    if (-not ((Get-Content -LiteralPath $versionHeader) -contains "#define XMJD_VERSION_FULL `"$version`"")) { throw "Build version differs: $arch" }
    $buildDirectories[$arch] = $buildDirectory
    Write-Output "Build and CTest passed: $arch $Configuration"
}
if ($Architecture -ne 'all') {
    Write-Output 'Use -Architecture all (the default) to build, test and package both architectures together.'
    return
}
New-Item -ItemType Directory -Force -Path $packageParent | Out-Null
$stagingRoot = Join-Path $packageParent ".staging-$([guid]::NewGuid().ToString('N'))"
$stagingZip = "$stagingRoot.zip"
try {
    foreach ($arch in $architectures) {
        & $cmake.Source --install $buildDirectories[$arch] --config $Configuration --prefix $stagingRoot
        if ($LASTEXITCODE -ne 0) { throw "CMake package install failed: $arch" }
    }
    $manifest = [ordered]@{
        formatVersion = 1
        product = 'XMJD'
        version = $version
        configuration = $Configuration
        architectures = @('x64', 'x86')
        createdUtc = [DateTime]::UtcNow.ToString('o')
        cmake = $cmakeVersion
        tests = $testEvidence
        lockSha256 = (Get-FileHash -LiteralPath (Join-Path $windowsRoot 'librime.lock.json') -Algorithm SHA256).Hash.ToLowerInvariant()
        schemeSha256 = $lock.scheme.sha256
        files = @(Get-PackageInventory $stagingRoot)
    }
    [IO.File]::WriteAllText((Join-Path $stagingRoot $manifestName), ($manifest | ConvertTo-Json -Depth 10), [Text.UTF8Encoding]::new($false))
    Assert-Package $stagingRoot
    # .NET Framework used by Windows PowerShell 5.1 writes backslashes in
    # CreateFromDirectory. Set each ZIP entry name explicitly to portable '/'.
    $zipStream = [IO.File]::Open($stagingZip, [IO.FileMode]::CreateNew)
    $archive = $null
    try {
        $archive = [IO.Compression.ZipArchive]::new($zipStream, [IO.Compression.ZipArchiveMode]::Create)
        foreach ($relative in (@($manifest.files | ForEach-Object { $_.path }) + @($manifestName))) {
            [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive,
                (Join-Path $stagingRoot $relative), $relative, [IO.Compression.CompressionLevel]::Optimal) | Out-Null
        }
    } finally {
        try { if ($archive) { $archive.Dispose() } }
        finally { $zipStream.Dispose() }
    }
    Assert-Package $stagingZip
    [IO.Directory]::Move($stagingRoot, $packageRoot)
    [IO.File]::Move($stagingZip, $zipPath)
} finally {
    if (Test-Path -LiteralPath $stagingRoot) { Remove-Item -LiteralPath $stagingRoot -Recurse -Force }
    if (Test-Path -LiteralPath $stagingZip) { Remove-Item -LiteralPath $stagingZip -Force }
}
Write-Output "Delivery directory: $packageRoot"
Write-Output "Delivery ZIP: $zipPath"
Write-Output "ZIP SHA-256: $((Get-FileHash -LiteralPath $zipPath -Algorithm SHA256).Hash.ToLowerInvariant())"
Write-Output 'No registration or installation into personal directories was performed.'
