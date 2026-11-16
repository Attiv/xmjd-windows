[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('x64', 'x86')]
    [string] $Architecture,
    [switch] $VerifyOnly
)

$ErrorActionPreference = 'Stop'
$windowsRoot = Split-Path -Parent $PSScriptRoot
$lockPath = Join-Path $windowsRoot 'librime.lock.json'
$lock = Get-Content -LiteralPath $lockPath -Raw | ConvertFrom-Json
$archLock = $lock.architectures.$Architecture
$luaPluginLock = $lock.plugins.'librime-lua'
$lockHash = (Get-FileHash -LiteralPath $lockPath -Algorithm SHA256).Hash.ToLowerInvariant()
$dependencyRoot = Join-Path $windowsRoot ".deps/librime/$($lock.version)/$Architecture"
$markerName = 'xmjd-librime-lock.json'
$markerPath = Join-Path $dependencyRoot $markerName

function Assert-RelativePath([string] $Path) {
    if ([string]::IsNullOrWhiteSpace($Path) -or $Path -match '[\\:\x00-\x1f<>"|?*]' -or
        $Path.StartsWith('/') -or $Path -match '(^|/)(\.|\.\.|)(/|$)') {
        throw "Unsafe relative path: $Path"
    }
    foreach ($part in $Path.Split('/')) {
        if ($part.EndsWith('.') -or $part.EndsWith(' ') -or
            $part -match '^(?i:CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\.|$)') {
            throw "Unsafe Windows path: $Path"
        }
    }
}

function Get-TreeFiles([string] $Root) {
    $rootItem = Get-Item -LiteralPath $Root -Force
    if (-not $rootItem.PSIsContainer -or ($rootItem.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        throw "Not an ordinary directory: $Root"
    }
    $pending = New-Object 'System.Collections.Generic.Stack[string]'
    $pending.Push($rootItem.FullName)
    while ($pending.Count -gt 0) {
        foreach ($item in (Get-ChildItem -LiteralPath $pending.Pop() -Force)) {
            if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Links are not allowed: $($item.FullName)" }
            if ($item.PSIsContainer) { $pending.Push($item.FullName) } else { $item }
        }
    }
}

function Get-ContentInventory([string] $Root) {
    $prefix = [IO.Path]::GetFullPath($Root).TrimEnd([char[]]@('/', '\')) + [IO.Path]::DirectorySeparatorChar
    foreach ($file in (Get-TreeFiles $Root | Sort-Object FullName)) {
        $relative = $file.FullName.Substring($prefix.Length).Replace('\', '/')
        Assert-RelativePath $relative
        if ($relative -eq $markerName) { continue }
        [ordered]@{
            path = $relative
            size = $file.Length
            sha256 = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        }
    }
}

function Assert-PreparedPackage {
    $marker = Get-Content -LiteralPath $markerPath -Raw | ConvertFrom-Json
    if ($marker.formatVersion -ne 2 -or $marker.lockSha256 -ne $lockHash -or
        $marker.version -ne $lock.version -or $marker.commit -ne $lock.commit -or
        $marker.architecture -ne $Architecture -or
        $marker.engineSha256 -ne $archLock.engine.sha256 -or
        $marker.dependenciesSha256 -ne $archLock.dependencies.sha256 -or
        $marker.luaPluginCommit -ne $luaPluginLock.commit -or $marker.luaVersion -ne $luaPluginLock.luaVersion) {
        throw "Dependency metadata differs from the lock: $dependencyRoot"
    }
    $actual = @(Get-ContentInventory $dependencyRoot)
    $expected = @{}
    foreach ($entry in $marker.files) {
        Assert-RelativePath $entry.path
        if ($expected.ContainsKey($entry.path)) { throw "Duplicate inventory path: $($entry.path)" }
        $expected[$entry.path] = $entry
    }
    if ($actual.Count -eq 0 -or $expected.Count -ne $actual.Count) { throw 'Dependency file inventory differs from the prepared package.' }
    foreach ($entry in $actual) {
        $reference = $expected[$entry.path]
        if (-not $reference -or $entry.size -ne $reference.size -or $entry.sha256 -ne $reference.sha256) {
            throw "Dependency content verification failed: $($entry.path)"
        }
    }
    foreach ($required in @('bin/rime.dll', 'include/rime_api.h', 'include/rime_api_stdbool.h',
        'version-info.txt', 'share/opencc/s2t.json', 'share/opencc/STCharacters.ocd2', 'licenses/librime-LICENSE')) {
        if (-not $expected.ContainsKey($required)) { throw "Missing dependency file: $required" }
    }
    foreach ($asset in $lock.dataAssets) {
        $entry = $expected["share/rime-data/$($asset.name)"]
        if (-not $entry -or $entry.sha256 -ne $asset.sha256) { throw "Data asset differs from lock: $($asset.name)" }
    }
    foreach ($license in $lock.licenses) {
        $entry = $expected["licenses/$($license.name)"]
        if (-not $entry -or $entry.sha256 -ne $license.sha256) { throw "License differs from lock: $($license.name)" }
    }
}

if (Test-Path -LiteralPath $dependencyRoot) {
    if (-not (Test-Path -LiteralPath $markerPath -PathType Leaf)) { throw "Refusing to overwrite an unverified directory: $dependencyRoot" }
    Assert-PreparedPackage
    Write-Output "Verified all prepared librime $($lock.version) $Architecture content hashes."
    return
}
if ($VerifyOnly) { throw "Prepared dependency directory is missing: $dependencyRoot" }

$sevenZip = Get-Command '7z.exe' -ErrorAction SilentlyContinue
if (-not $sevenZip) { $sevenZip = Get-Command '7z' -ErrorAction SilentlyContinue }
if (-not $sevenZip) { throw '7-Zip (7z.exe) is required to unpack the pinned official .7z archives.' }
[Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
$cacheRoot = Join-Path $windowsRoot ".deps/cache/librime/$($lock.version)/$Architecture"
$stageRoot = Join-Path $windowsRoot ".deps/staging/librime-$($lock.version)-$Architecture-$([guid]::NewGuid().ToString('N'))"
$engineExtract = Join-Path $stageRoot 'engine'
$dependenciesExtract = Join-Path $stageRoot 'dependencies'
New-Item -ItemType Directory -Force -Path $cacheRoot, $engineExtract, $dependenciesExtract | Out-Null

function Get-VerifiedAsset([object] $Asset, [string] $Cache) {
    Assert-RelativePath $Asset.name
    if ($Asset.name.Contains('/')) { throw 'Asset names must be file names.' }
    if ($Asset.url -notmatch '^https://' -or $Asset.sha256 -notmatch '^[a-f0-9]{64}$') { throw 'Invalid locked asset.' }
    New-Item -ItemType Directory -Force -Path $Cache | Out-Null
    $path = Join-Path $Cache $Asset.name
    if (-not (Test-Path -LiteralPath $path)) {
        $download = "$path.download-$([guid]::NewGuid().ToString('N'))"
        try {
            Invoke-WebRequest -Uri $Asset.url -OutFile $download -UseBasicParsing
            if ((Get-FileHash -LiteralPath $download -Algorithm SHA256).Hash.ToLowerInvariant() -ne $Asset.sha256) {
                throw "SHA-256 mismatch for downloaded $($Asset.name)"
            }
            Move-Item -LiteralPath $download -Destination $path
        } finally {
            if (Test-Path -LiteralPath $download) { Remove-Item -LiteralPath $download -Force }
        }
    }
    if ((Get-Item -LiteralPath $path -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Linked cache file: $path" }
    $actualHash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actualHash -ne $Asset.sha256) { throw "SHA-256 mismatch for cached $($Asset.name): $actualHash" }
    return $path
}

function Expand-SafeArchive([string] $Archive, [string] $Destination) {
    $listing = @(& $sevenZip.Source l -slt -ba -sccUTF-8 -bsp0 -- $Archive)
    if ($LASTEXITCODE -ne 0) { throw "7-Zip could not inspect $Archive" }
    $entries = @{}
    $files = @{}
    foreach ($block in (($listing -join "`n") -split '\n\s*\n')) {
        if ([string]::IsNullOrWhiteSpace($block)) { continue }
        $fields = @{}
        foreach ($line in ($block -split '\n')) {
            if ($line -match '^([^=]+) = (.*)$') {
                $key = $Matches[1].Trim()
                if ($fields.ContainsKey($key)) { throw 'Ambiguous archive listing.' }
                $fields[$key] = $Matches[2].TrimEnd("`r")
            }
        }
        if (-not $fields.ContainsKey('Path')) { throw 'Unexpected archive listing.' }
        $path = $fields.Path.Replace('\', '/').TrimEnd('/')
        Assert-RelativePath $path
        if ($entries.ContainsKey($path)) { throw "Duplicate archive path: $path" }
        if ($fields.ContainsKey('Symbolic Link') -or $fields.ContainsKey('Hard Link') -or
            $fields.ContainsKey('Reparse') -or $fields.Attributes -match '(?i)reparse|(^|\s)l|^L| l[rwx-]{9}' -or
            $fields.Mode -match '^l' -or $fields.Encrypted -eq '+') {
            throw "Links, reparse points and encrypted entries are not allowed: $path"
        }
        $isDirectory = $fields.Folder -eq '+' -or $fields.Attributes -match '^D|^d| d[rwx-]{9}'
        $entries[$path] = $isDirectory
        if (-not $isDirectory) {
            if ($fields.Size -notmatch '^\d+$') { throw "Missing archive size: $path" }
            $files[$path] = [long] $fields.Size
        }
    }
    if ($files.Count -eq 0) { throw 'Archive contains no files.' }
    foreach ($path in $entries.Keys) {
        $parent = $path
        while ($parent.Contains('/')) {
            $parent = $parent.Substring(0, $parent.LastIndexOf('/'))
            if ($entries.ContainsKey($parent) -and -not $entries[$parent]) { throw "Archive file is also a parent: $parent" }
        }
    }
    & $sevenZip.Source x -y -sccUTF-8 -bsp0 "-o$Destination" -- $Archive | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "7-Zip failed to extract $Archive" }
    $prefix = [IO.Path]::GetFullPath($Destination).TrimEnd([char[]]@('/', '\')) + [IO.Path]::DirectorySeparatorChar
    $extracted = @(Get-TreeFiles $Destination)
    if ($extracted.Count -ne $files.Count) { throw 'Extracted archive inventory differs from listing.' }
    foreach ($file in $extracted) {
        $relative = $file.FullName.Substring($prefix.Length).Replace('\', '/')
        if (-not $files.ContainsKey($relative) -or $file.Length -ne $files[$relative]) { throw "Unexpected extracted file: $relative" }
    }
}

function Copy-UniqueFile([string] $Source, [string] $Destination) {
    if (Test-Path -LiteralPath $Destination) {
        if ((Get-FileHash -LiteralPath $Source -Algorithm SHA256).Hash -ne
            (Get-FileHash -LiteralPath $Destination -Algorithm SHA256).Hash) { throw "Conflicting package file: $Destination" }
        return
    }
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Destination) | Out-Null
    Copy-Item -LiteralPath $Source -Destination $Destination
}

function Copy-Tree([string] $Source, [string] $Destination) {
    if (-not (Test-Path -LiteralPath $Source -PathType Container)) { return }
    $prefix = [IO.Path]::GetFullPath($Source).TrimEnd([char[]]@('/', '\')) + [IO.Path]::DirectorySeparatorChar
    foreach ($file in (Get-TreeFiles $Source)) {
        Copy-UniqueFile $file.FullName (Join-Path $Destination $file.FullName.Substring($prefix.Length))
    }
}

try {
    $engineArchive = Get-VerifiedAsset $archLock.engine $cacheRoot
    $dependenciesArchive = Get-VerifiedAsset $archLock.dependencies $cacheRoot
    Expand-SafeArchive $engineArchive $engineExtract
    Expand-SafeArchive $dependenciesArchive $dependenciesExtract
    foreach ($required in @('dist/include/rime_api.h', 'dist/lib/rime.dll', 'version-info.txt')) {
        if (-not (Test-Path -LiteralPath (Join-Path $engineExtract $required) -PathType Leaf)) { throw "Unexpected engine layout: $required" }
    }
    $versionInfoPath = Join-Path $engineExtract 'version-info.txt'
    $versionInfo = (Get-Content -LiteralPath $versionInfoPath -Raw) -replace '\r', ''
    foreach ($plugin in $lock.plugins.PSObject.Properties) {
        if ($versionInfo -notmatch ([regex]::Escape($plugin.Name) + '\s+' + $plugin.Value.commit.Substring(0, 7) + '(\s|$)')) {
            throw "Official version metadata differs from locked plugin: $($plugin.Name)"
        }
    }
    if ($versionInfo -notmatch ('librime\s+' + $lock.commit.Substring(0, 7) + '(\s|$)')) { throw 'Official librime version metadata differs from lock.' }
    $dllPath = Join-Path $engineExtract 'dist/lib/rime.dll'
    $dllBytes = [IO.File]::ReadAllBytes($dllPath)
    $peOffset = [BitConverter]::ToInt32($dllBytes, 0x3c)
    $machine = [BitConverter]::ToUInt16($dllBytes, $peOffset + 4)
    $expectedMachine = if ($Architecture -eq 'x64') { 0x8664 } else { 0x014c }
    if ($machine -ne $expectedMachine) { throw 'Official rime.dll architecture differs from lock.' }
    if (-not [Text.Encoding]::ASCII.GetString($dllBytes).Contains("`$LuaVersion: Lua $($luaPluginLock.luaVersion) ")) { throw 'Embedded Lua version differs from lock.' }
    $openccInfo = Get-Content -LiteralPath (Join-Path $dependenciesExtract 'lib/pkgconfig/opencc.pc') -Raw
    if ($openccInfo -notmatch ('(?m)^Version: ' + [regex]::Escape($lock.nativeDependencies.OpenCC.version) + '\s*$')) { throw 'OpenCC version differs from lock.' }

    $packageRoot = Join-Path $stageRoot 'package'
    New-Item -ItemType Directory -Force -Path $packageRoot | Out-Null
    Copy-Tree (Join-Path $engineExtract 'dist/include') (Join-Path $packageRoot 'include')
    Copy-Tree (Join-Path $dependenciesExtract 'include') (Join-Path $packageRoot 'include')
    Copy-Tree (Join-Path $engineExtract 'dist/share') (Join-Path $packageRoot 'share')
    Copy-Tree (Join-Path $dependenciesExtract 'share') (Join-Path $packageRoot 'share')
    Copy-UniqueFile $versionInfoPath (Join-Path $packageRoot 'version-info.txt')
    foreach ($directory in @('dist/bin', 'dist/lib')) {
        foreach ($file in (Get-ChildItem -LiteralPath (Join-Path $engineExtract $directory) -Filter '*.dll' -File)) {
            Copy-UniqueFile $file.FullName (Join-Path $packageRoot "bin/$($file.Name)")
        }
    }
    foreach ($directory in @('bin', 'lib')) {
        foreach ($file in (Get-ChildItem -LiteralPath (Join-Path $dependenciesExtract $directory) -Filter '*.dll' -File)) {
            Copy-UniqueFile $file.FullName (Join-Path $packageRoot "bin/$($file.Name)")
        }
    }
    foreach ($source in @($engineExtract, $dependenciesExtract)) {
        $prefix = $source + [IO.Path]::DirectorySeparatorChar
        $label = Split-Path -Leaf $source
        foreach ($file in (Get-TreeFiles $source)) {
            if ($file.Name -match '^(?i:LICENSE|COPYING|NOTICE|COPYRIGHT)(\.|$)') {
                Copy-UniqueFile $file.FullName (Join-Path $packageRoot "licenses/archive-$label/$($file.FullName.Substring($prefix.Length))")
            }
        }
    }
    foreach ($header in @('utf8.h', 'X11/keysym.h', 'X11/keysymdef.h')) {
        Copy-UniqueFile (Join-Path $dependenciesExtract "include/$header") (Join-Path $packageRoot "licenses/header-notices/$header")
    }
    foreach ($license in $lock.licenses) {
        $cachedLicense = Get-VerifiedAsset $license (Join-Path $cacheRoot 'licenses')
        Copy-UniqueFile $cachedLicense (Join-Path $packageRoot "licenses/$($license.name)")
    }
    foreach ($asset in $lock.dataAssets) {
        $cachedAsset = Get-VerifiedAsset $asset (Join-Path $cacheRoot 'data')
        Copy-UniqueFile $cachedAsset (Join-Path $packageRoot "share/rime-data/$($asset.name)")
    }
    $marker = [ordered]@{
        formatVersion = 2
        lockSha256 = $lockHash
        source = $lock.source
        version = $lock.version
        commit = $lock.commit
        luaPluginCommit = $luaPluginLock.commit
        luaVersion = $luaPluginLock.luaVersion
        architecture = $Architecture
        engineSha256 = $archLock.engine.sha256
        dependenciesSha256 = $archLock.dependencies.sha256
        files = @(Get-ContentInventory $packageRoot)
    }
    [IO.File]::WriteAllText((Join-Path $packageRoot $markerName),
        ($marker | ConvertTo-Json -Depth 8), [Text.UTF8Encoding]::new($false))
    New-Item -ItemType Directory -Path (Split-Path -Parent $dependencyRoot) -Force | Out-Null
    Move-Item -LiteralPath $packageRoot -Destination $dependencyRoot
    Assert-PreparedPackage
    Write-Output "Prepared and verified librime $($lock.version) $Architecture at $dependencyRoot"
    Write-Output 'The Host must still pass find_module(lua) at runtime; provenance is not a readiness check.'
}
finally {
    if (Test-Path -LiteralPath $stageRoot) { Remove-Item -LiteralPath $stageRoot -Recurse -Force }
}
