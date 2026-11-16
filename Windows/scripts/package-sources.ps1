#requires -Version 5.1
[CmdletBinding()]
param(
    [string] $OutputDirectory,
    [string] $WorkDirectory,
    [string] $CacheDirectory
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0
$windowsRoot = Split-Path -Parent $PSScriptRoot
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $windowsRoot 'out/release' }
if (-not $WorkDirectory) { $WorkDirectory = Join-Path $windowsRoot '.deps/staging/sources' }
if (-not $CacheDirectory) { $CacheDirectory = Join-Path $windowsRoot '.deps/cache/sources' }
$lockPath = Join-Path $windowsRoot 'librime.lock.json'
$sourceLockPath = Join-Path $windowsRoot 'librime-sources.lock.json'
$lock = Get-Content -LiteralPath $lockPath -Raw -Encoding UTF8 | ConvertFrom-Json
$sourceLock = Get-Content -LiteralPath $sourceLockPath -Raw -Encoding UTF8 | ConvertFrom-Json
$git = (Get-Command git -ErrorAction Stop).Source
$utf8 = New-Object Text.UTF8Encoding($false)

function Get-OptionalProperty([object] $Object, [string] $Name) {
    $property = $Object.PSObject.Properties[$Name]
    if ($null -ne $property) { return $property.Value }
    return $null
}

function Assert-RelativePath([string] $Path) {
    if ([string]::IsNullOrWhiteSpace($Path) -or $Path -match '[\\:\x00-\x1f<>"|?*]' -or
        $Path.StartsWith('/') -or $Path -match '(^|/)(\.|\.\.|)(/|$)') {
        throw "不安全的相对路径：$Path"
    }
    foreach ($part in $Path.Split('/')) {
        if ($part.EndsWith('.') -or $part.EndsWith(' ') -or
            $part -match '^(?i:CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\.|$)') {
            throw "不安全的 Windows 路径：$Path"
        }
    }
}

function Get-Sha256([string] $Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Write-Utf8([string] $Path, [string] $Content) {
    [IO.File]::WriteAllText($Path, $Content, $utf8)
}

function Invoke-Git([string[]] $Arguments) {
    $result = @(& $git -c "core.hooksPath=$hookRoot" -c protocol.allow=never -c protocol.https.allow=always `
        -c fetch.recurseSubmodules=false -c credential.interactive=never `
        -c http.lowSpeedLimit=1024 -c http.lowSpeedTime=60 @Arguments)
    if ($LASTEXITCODE -ne 0) { throw "Git 操作失败：$($Arguments -join ' ')" }
    return $result
}

function Get-VerifiedAsset([object] $Asset) {
    Assert-RelativePath $Asset.name
    if ($Asset.name.Contains('/') -or $Asset.url -notmatch '^https://' -or
        $Asset.sha256 -notmatch '^[a-f0-9]{64}$') { throw '下载锁定信息无效。' }
    $maxBytes = Get-OptionalProperty $Asset 'maxBytes'
    if (-not $maxBytes) { $maxBytes = 1MB }
    if ($maxBytes -gt 200MB -or $maxBytes -lt 1) { throw '下载大小上限无效。' }
    $path = Join-Path $CacheDirectory $Asset.name
    if (-not (Test-Path -LiteralPath $path)) {
        $temporary = "$path.download-$([guid]::NewGuid().ToString('N'))"
        $response = $null
        $inputStream = $null
        $outputStream = $null
        try {
            $request = [Net.HttpWebRequest]::Create($Asset.url)
            $request.UserAgent = 'xmjd-corresponding-sources/1'
            $request.Timeout = 60000
            $request.ReadWriteTimeout = 60000
            $response = $request.GetResponse()
            if ($response.ResponseUri.Scheme -ne 'https' -or $response.ContentLength -gt $maxBytes) {
                throw "下载协议或大小不符：$($Asset.name)"
            }
            $inputStream = $response.GetResponseStream()
            $outputStream = [IO.File]::Create($temporary)
            $buffer = New-Object byte[] 65536
            $total = 0L
            $clock = [Diagnostics.Stopwatch]::StartNew()
            while (($count = $inputStream.Read($buffer, 0, $buffer.Length)) -gt 0) {
                $total += $count
                if ($total -gt $maxBytes -or $clock.Elapsed.TotalMinutes -gt 15) {
                    throw "下载超过大小或时长上限：$($Asset.name)"
                }
                $outputStream.Write($buffer, 0, $count)
            }
            $outputStream.Dispose()
            $outputStream = $null
            if ((Get-Sha256 $temporary) -ne $Asset.sha256) { throw "下载 SHA-256 不符：$($Asset.name)" }
            Move-Item -LiteralPath $temporary -Destination $path
        } finally {
            if ($null -ne $outputStream) { $outputStream.Dispose() }
            if ($null -ne $inputStream) { $inputStream.Dispose() }
            if ($null -ne $response) { $response.Dispose() }
            if (Test-Path -LiteralPath $temporary) { Remove-Item -LiteralPath $temporary -Force }
        }
    }
    $item = Get-Item -LiteralPath $path -Force
    if ($item.PSIsContainer -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -or
        $item.Length -gt $maxBytes -or (Get-Sha256 $path) -ne $Asset.sha256) {
        throw "缓存文件校验失败：$path"
    }
    return $path
}

if ($sourceLock.formatVersion -ne 1 -or $sourceLock.librimeCommit -ne $lock.commit -or
    $lock.version -notmatch '^\d+\.\d+\.\d+$') { throw '源码锁与二进制锁不匹配。' }
$repositories = @($sourceLock.repositories)
if ($repositories.Count -eq 0 -or $repositories.Count -gt 32) { throw '源码仓库数量超出范围。' }
$byId = @{}
$byPath = @{}
foreach ($repository in $repositories) {
    Assert-RelativePath $repository.path
    if ($repository.id -notmatch '^[a-z0-9][a-z0-9-]*$' -or
        $repository.source -notmatch '^https://github\.com/[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+\.git$' -or
        $repository.commit -notmatch '^[a-f0-9]{40}$' -or
        $byId.ContainsKey($repository.id) -or $byPath.ContainsKey($repository.path)) {
        throw '源码仓库锁定信息无效或重复。'
    }
    $lockKey = Get-OptionalProperty $repository 'lockKey'
    if ($lockKey) {
        $value = $lock
        foreach ($key in $lockKey.Split('.')) { $value = $value.$key }
        if ($value -ne $repository.commit) { throw "提交与二进制锁不一致：$($repository.id)" }
    }
    $byId[$repository.id] = $repository
    $byPath[$repository.path] = $repository
}
if (($byId['librime'].source -replace '\.git$', '') -ne $lock.source.TrimEnd('/')) {
    throw 'librime 来源与二进制锁不一致。'
}
foreach ($plugin in $lock.plugins.PSObject.Properties) {
    if (-not $byId.ContainsKey($plugin.Name)) { throw "缺少插件源码：$($plugin.Name)" }
}
foreach ($asset in $sourceLock.archives) {
    if ($asset.version -ne $lock.nativeDependencies.($asset.id).version) { throw "源码版本与二进制锁不一致：$($asset.id)" }
}
if ($byId['lua-thirdparty'].version -ne $lock.plugins.'librime-lua'.luaVersion) { throw 'Lua 供应源码版本不匹配。' }

$baseName = "xmjd-windows-librime-$($lock.version)-sources"
$archiveName = "$baseName.zip"
$provenanceName = "$baseName.provenance.json"
$hashName = "$baseName.SHA256SUMS.txt"
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
$WorkDirectory = [IO.Path]::GetFullPath($WorkDirectory)
$CacheDirectory = [IO.Path]::GetFullPath($CacheDirectory)
foreach ($directory in @($OutputDirectory, $WorkDirectory, $CacheDirectory)) {
    New-Item -ItemType Directory -Path $directory -Force | Out-Null
    if ((Get-Item -LiteralPath $directory -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) {
        throw "目录不能是链接：$directory"
    }
}
foreach ($name in @($archiveName, $provenanceName, $hashName)) {
    if (Test-Path -LiteralPath (Join-Path $OutputDirectory $name)) { throw "拒绝覆盖已有交付物：$name" }
}
[Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
$stageRoot = Join-Path $WorkDirectory ([guid]::NewGuid().ToString('N'))
$packageRoot = Join-Path $stageRoot $baseName
$gitRoot = Join-Path $stageRoot 'repositories'
$hookRoot = Join-Path $stageRoot 'empty-hooks'
$archivesRoot = Join-Path $packageRoot 'archives'
$licensesRoot = Join-Path $packageRoot 'licenses'
New-Item -ItemType Directory -Path $packageRoot, $gitRoot, $hookRoot, $archivesRoot, $licensesRoot -Force | Out-Null
$repositoryRecords = @()
$assetRecords = @()
try {
    foreach ($repository in $repositories) {
        Write-Output "获取固定源码：$($repository.id) $($repository.commit)"
        $bare = Join-Path $gitRoot "$($repository.id).git"
        Invoke-Git @('init', '--bare', '--quiet', $bare) | Out-Null
        Invoke-Git @('-C', $bare, 'remote', 'add', 'origin', $repository.source) | Out-Null
        Invoke-Git @('-C', $bare, 'fetch', '--quiet', '--depth=1', '--no-tags', '--no-recurse-submodules', 'origin', $repository.commit) | Out-Null
        $actualCommit = (Invoke-Git @('-C', $bare, 'rev-parse', 'FETCH_HEAD')) -join ''
        if ($actualCommit -ne $repository.commit) { throw "Git 提交校验失败：$($repository.id)" }
        Invoke-Git @('-C', $bare, 'fsck', '--strict', '--no-progress', '--no-reflogs', $actualCommit) | Out-Null
        $tree = (Invoke-Git @('-C', $bare, 'rev-parse', "$actualCommit`^{tree}")) -join ''
        $name = "$($repository.id)-$actualCommit.tar"
        # git archive 直接写二进制文件，避免 PowerShell 5.1 管道重编码。
        Invoke-Git @('-C', $bare, 'archive', '--format=tar', "--output=$(Join-Path $archivesRoot $name)", $actualCommit) | Out-Null
        $repositoryRecords += [ordered]@{
            id = $repository.id; path = $repository.path; source = $repository.source
            commit = $actualCommit; tree = $tree; archive = "archives/$name"
            sha256 = Get-Sha256 (Join-Path $archivesRoot $name)
        }
    }

    # 每个 gitlink 必须在白名单中出现；逐级检查，不能遗漏递归子模块。
    foreach ($repository in $repositories) {
        $bare = Join-Path $gitRoot "$($repository.id).git"
        $gitlinks = @{}
        $treeLines = Invoke-Git @('-C', $bare, 'ls-tree', '-r', '--full-tree', $repository.commit)
        foreach ($line in $treeLines) {
            if ($line -match '^160000 commit ([a-f0-9]{40})\t(.+)$') { $gitlinks[$Matches[2]] = $Matches[1] }
        }
        $children = @($repositories | Where-Object { (Get-OptionalProperty $_ 'parent') -eq $repository.id })
        if ($children.Count -ne $gitlinks.Count) { throw "子模块清单不完整：$($repository.id)" }
        $modulePaths = @{}
        if ($gitlinks.Count -gt 0) {
            $moduleLines = Invoke-Git @('-C', $bare, 'config', '--blob', "$($repository.commit):.gitmodules", '--get-regexp', '^submodule\..*\.path$')
            foreach ($line in $moduleLines) {
                if ($line -notmatch '^(submodule\..*)\.path (.+)$') { throw '无法识别 .gitmodules。' }
                $modulePaths[$Matches[2]] = "$($Matches[1]).url"
            }
        }
        if ($modulePaths.Count -ne $gitlinks.Count) { throw "gitlink 与 .gitmodules 不一致：$($repository.id)" }
        foreach ($child in $children) {
            Assert-RelativePath $child.submodulePath
            if ($gitlinks[$child.submodulePath] -ne $child.commit -or
                $child.path -ne "$($repository.path)/$($child.submodulePath)" -or
                -not $modulePaths.ContainsKey($child.submodulePath)) { throw "子模块提交或路径不符：$($child.id)" }
            $url = (Invoke-Git @('-C', $bare, 'config', '--blob', "$($repository.commit):.gitmodules", '--get', $modulePaths[$child.submodulePath])) -join ''
            if (($url -replace '\.git$', '') -ne ($child.source -replace '\.git$', '')) { throw "子模块来源不符：$($child.id)" }
        }
    }

    foreach ($asset in $sourceLock.archives) {
        $download = Get-VerifiedAsset $asset
        Copy-Item -LiteralPath $download -Destination (Join-Path $archivesRoot $asset.name)
        $assetRecords += [ordered]@{
            id = $asset.id; version = $asset.version; url = $asset.url
            archive = "archives/$($asset.name)"; sha256 = $asset.sha256
            checksumEvidence = $asset.checksumEvidence
        }
    }
    $licenseRecords = @()
    foreach ($license in @($lock.licenses) + @($sourceLock.additionalLicenses)) {
        $download = Get-VerifiedAsset $license
        Copy-Item -LiteralPath $download -Destination (Join-Path $licensesRoot $license.name)
        $licenseRecords += [ordered]@{ path = "licenses/$($license.name)"; url = $license.url; sha256 = $license.sha256 }
    }
    foreach ($file in @($lockPath, $sourceLockPath, (Join-Path $windowsRoot 'THIRD_PARTY_NOTICES.txt'))) {
        Copy-Item -LiteralPath $file -Destination $packageRoot
    }
    $scriptsRoot = Join-Path $packageRoot 'scripts'
    New-Item -ItemType Directory -Path $scriptsRoot | Out-Null
    Copy-Item -LiteralPath $PSCommandPath -Destination $scriptsRoot

    $rebuild = @'
#requires -Version 5.1
[CmdletBinding()]
param([string] $Destination = (Join-Path $PSScriptRoot 'source'))
$ErrorActionPreference = 'Stop'
$tar = (Get-Command tar -ErrorAction Stop).Source
if (Test-Path -LiteralPath $Destination) { throw '目标已存在，请使用新的空目录。' }
$manifest = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'PROVENANCE.json') -Raw -Encoding UTF8 | ConvertFrom-Json
foreach ($entry in $manifest.files) {
    $path = Join-Path $PSScriptRoot $entry.path
    if ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() -ne $entry.sha256) {
        throw "源码文件校验失败：$($entry.path)"
    }
}
New-Item -ItemType Directory -Path $Destination | Out-Null
foreach ($repository in $manifest.repositories) {
    $target = Join-Path $Destination $repository.path
    New-Item -ItemType Directory -Path $target -Force | Out-Null
    & $tar -xf (Join-Path $PSScriptRoot $repository.archive) -C $target
    if ($LASTEXITCODE -ne 0) { throw "源码解包失败：$($repository.id)" }
}
foreach ($asset in $manifest.upstreamArchives) {
    & $tar -xf (Join-Path $PSScriptRoot $asset.archive) -C $Destination
    if ($LASTEXITCODE -ne 0) { throw "源码解包失败：$($asset.id)" }
}
Write-Output "已校验并解包源码：$Destination"
'@
    # Windows PowerShell 5.1 以 BOM 识别含中文的脚本。
    [IO.File]::WriteAllText((Join-Path $packageRoot 'Expand-Sources.ps1'), $rebuild, (New-Object Text.UTF8Encoding($true)))
    $instructions = @'
第三方对应源码与构建说明

本包随 Windows librime 1.16.1 官方 MSVC 引擎提供，适用于 x64/x86。
archives 中包含固定 Git 提交的完整源码 tar（包含隐藏文件、构建脚本、许可证），
以及官方完整 Lua 5.4.8、Boost 1.89.0 源码归档，不需要在线获取源码。
每个递归子模块各有独立归档，PROVENANCE.json 记录其原路径、来源、提交、tree 和 SHA-256。
Git 归档不包含 .git 历史；归档内容及内嵌第三方目录均原样保留。

1. 使用 PowerShell 5.1 或更新版本执行：
   powershell -NoProfile -ExecutionPolicy Bypass -File .\Expand-Sources.ps1
   该脚本先检查文件哈希，然后使用 Windows tar 解包。只解包，不构建。

2. 在 Visual Studio 2022 的 x64 或 x86 Native Tools Command Prompt 中构建。
   需要 MSVC v143、Windows SDK、CMake、Ninja 和 Python（OpenCC 字典生成）。
   先在 source\librime 内新建 ASCII 编码的 env.bat，内容如下：
   set RIME_ROOT=%CD%
   set BOOST_ROOT=%RIME_ROOT%\..\boost_1_89_0
   set CC=cl
   set CXX=cl
   set CMAKE_GENERATOR=Ninja

   必须创建此文件，避免 build.bat 自动复制仍指定 VS2019/v142 的旧模板。
   然后在命令提示符执行：
   cd source\librime
   call env.bat
   pushd ..\boost_1_89_0
   call bootstrap.bat
   b2 headers
   popd
   call build.bat deps
   call build.bat test

   不要运行 install-boost.bat、action-install-plugins-windows.bat 或 Lua 的
   action-install.bat；本包已提供对应版本，以上脚本会重新下载或追踪浮动分支。
   build.bat、env.bat.template、cmake/* 和 .github/workflows/windows-build.yml
   均在原始 librime 归档中，可检查上游参数及工具链设置。

3. 三个插件已分别放入 plugins\lua、plugins\octagram、plugins\predict；
   CMake 默认 BUILD_MERGED_PLUGINS=ON。Lua 使用 thirdparty\lua5.4，
   原插件构建参数含 LUA_COMPAT_5_3、Windows LUA_BUILD_AS_DLL。
   thirdparty 固定在 fa40fadd8af1e5b1fbd55703ccbd54476956d74c：
   它是官方引擎发布前该分支的最后一次提交。lua5.4 与官方 5.4.8 比较，
   除 luaconf.h 的供应方修改和未包含独立解释器 lua.c/luac.c 外一致。
   完整原版（包括解释器、Makefile、文档与 MIT 许可）在独立 Lua 归档中。

4. octagram 固定提交使用 librime、Boost、Darts，并不使用 KenLM。
   LevelDB 的 benchmark/googletest 递归子模块全部提供，尽管上游 Windows
   build.bat 关闭 LevelDB benchmarks/tests。OpenCC 的内嵌 marisa、Darts、
   RapidJSON、TCLAP、GoogleTest、benchmark、pybind11 均随 OpenCC 归档保留；
   yaml-cpp 自带的 GoogleTest、librime 的 UTF8-CPP/X11/Darts 也完整保留。
   glog 的上游 Windows 构建关闭 gflags；LevelDB 的可选 snappy/crc32c
   在标准官方 MSVC 依赖流程中未单独安装，不应自行添加不同版本。

5. 这是第三方源码交付，不声称能够逐字节复现上游预编译 DLL；上游工具链补丁
   版本、缓存及编译时间可能影响二进制。正式二进制及其校验值在 librime.lock.json。
   PROVENANCE 的 binaryArtifacts 同时记录两个架构的引擎与依赖校验值。
   xmjd Windows 自有源码及项目构建/安装脚本由同一发布版本的项目源码附件提供，
   本包不为这些代码制定或变更许可。第三方的原始版权与许可文本优先。

发布者应将本源码 ZIP、provenance JSON 和 SHA256SUMS 与安装程序置于同一版本
的发布附件，提供同等下载访问，不得只保留上游源码链接。包含 GPLv3 octagram
的组合 rime.dll 的再分发须遵守相应 GPLv3 条件。licenses/librime-octagram-LICENSE
包含完整 GPLv3 文本。此包保留所有上游许可及构建脚本，不执行下载源码。
'@
    Write-Utf8 (Join-Path $packageRoot 'BUILD-SOURCES.txt') $instructions
    $inventory = @()
    foreach ($file in (Get-ChildItem -LiteralPath $packageRoot -Recurse -Force -File | Sort-Object FullName)) {
        $relative = $file.FullName.Substring($packageRoot.Length + 1).Replace('\', '/')
        Assert-RelativePath $relative
        $inventory += [ordered]@{ path = $relative; size = $file.Length; sha256 = Get-Sha256 $file.FullName }
    }
    $provenance = [ordered]@{
        formatVersion = 1; createdUtc = [DateTime]::UtcNow.ToString('o')
        librimeVersion = $lock.version; librimeCommit = $lock.commit
        binaryLockSha256 = Get-Sha256 $lockPath; sourceLockSha256 = Get-Sha256 $sourceLockPath
        binaryArtifacts = $lock.architectures; repositories = $repositoryRecords
        upstreamArchives = $assetRecords; licenses = $licenseRecords
        buildEvidence = $sourceLock.buildEvidence; files = $inventory
    }
    $provenanceJson = $provenance | ConvertTo-Json -Depth 12
    Write-Utf8 (Join-Path $packageRoot 'PROVENANCE.json') $provenanceJson
    Write-Utf8 (Join-Path $stageRoot $provenanceName) $provenanceJson
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zipPath = Join-Path $stageRoot $archiveName
    [IO.Compression.ZipFile]::CreateFromDirectory($packageRoot, $zipPath, [IO.Compression.CompressionLevel]::Optimal, $true)
    $zip = [IO.Compression.ZipFile]::OpenRead($zipPath)
    try {
        if ($zip.Entries.Count -ne $inventory.Count + 1) { throw '生成的 ZIP 文件清单不完整。' }
        foreach ($entry in $zip.Entries) { Assert-RelativePath ($entry.FullName.Replace('\', '/')) }
    } finally { $zip.Dispose() }
    Write-Utf8 (Join-Path $stageRoot $hashName) ((Get-Sha256 $zipPath) + "  $archiveName`n" +
        (Get-Sha256 (Join-Path $stageRoot $provenanceName)) + "  $provenanceName`n")
    foreach ($name in @($archiveName, $provenanceName, $hashName)) {
        Move-Item -LiteralPath (Join-Path $stageRoot $name) -Destination (Join-Path $OutputDirectory $name)
    }
    Write-Output "已生成并校验对应源码、来源清单与 SHA-256：$OutputDirectory"
} finally {
    if (Test-Path -LiteralPath $stageRoot) { Remove-Item -LiteralPath $stageRoot -Recurse -Force }
}
