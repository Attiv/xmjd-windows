[CmdletBinding()]
param(
    [string] $PackageDirectory,
    [string] $IsccPath,
    [switch] $VerifyOnly
)

$ErrorActionPreference = 'Stop'
$windowsRoot = Split-Path -Parent $PSScriptRoot
$innoVersion = '6.5.4'
$innoInstallerUrl = 'https://github.com/jrsoftware/issrc/releases/download/is-6_5_4/innosetup-6.5.4.exe'
$innoInstallerSha256 = 'fa73bf47a4da250d185d07561c2bfda387e5e20db77e4570004cf6a133cc10b1'

$versionParts = @{}
foreach ($line in (Get-Content -LiteralPath (Join-Path $windowsRoot 'VERSION'))) {
    if ($line -notmatch '^(MAJOR|MINOR|PATCH|BUILD)=([0-9]+)$') { throw 'Windows/VERSION 格式错误。' }
    $part = $Matches[1]
    if ($versionParts.ContainsKey($part) -or [long] $Matches[2] -gt 65535) { throw "Windows/VERSION 字段无效：$part" }
    $versionParts[$part] = $Matches[2]
}
foreach ($part in @('MAJOR', 'MINOR', 'PATCH', 'BUILD')) {
    if (-not $versionParts.ContainsKey($part)) { throw "Windows/VERSION 缺少 $part。" }
}
$version = '{0}.{1}.{2}+{3}' -f $versionParts.MAJOR, $versionParts.MINOR, $versionParts.PATCH, $versionParts.BUILD
$fileVersion = '{0}.{1}.{2}.{3}' -f $versionParts.MAJOR, $versionParts.MINOR, $versionParts.PATCH, $versionParts.BUILD
if (-not $PackageDirectory) {
    $PackageDirectory = Join-Path $windowsRoot "out/package/XMJD-$version-windows-x64-x86-Release"
}
$packageRoot = (Resolve-Path -LiteralPath $PackageDirectory).Path
if (-not (Test-Path -LiteralPath $packageRoot -PathType Container)) { throw 'PackageDirectory 必须是双架构交付目录。' }
& (Join-Path $PSScriptRoot 'build.ps1') -VerifyPackage $packageRoot
$manifest = Get-Content -LiteralPath (Join-Path $packageRoot 'manifest.json') -Raw | ConvertFrom-Json
if ($manifest.product -ne 'XMJD' -or $manifest.version -ne $version -or $manifest.configuration -ne 'Release' -or
    ($manifest.architectures -join ',') -ne 'x64,x86') {
    throw '只能打包与 Windows/VERSION 一致的 XMJD x64/x86 Release 交付目录。'
}
if (Compare-Object (Get-Content -LiteralPath (Join-Path $windowsRoot 'VERSION')) (Get-Content -LiteralPath (Join-Path $packageRoot 'VERSION'))) {
    throw '交付目录中的 VERSION 与 Windows/VERSION 不一致。'
}
foreach ($architecture in @('x64', 'x86')) {
    $evidence = @($manifest.tests | Where-Object { $_.architecture -eq $architecture })
    if ($evidence.Count -ne 1 -or $evidence[0].result -ne 'passed' -or -not $evidence[0].tests) {
        throw "交付清单缺少 $architecture 构建测试通过记录。"
    }
}
if ($VerifyOnly) {
    Write-Output "安装器输入验证通过：XMJD $version，双架构 Release，清单及全部文件 SHA-256 一致。"
    return
}
if ([Environment]::OSVersion.Platform -ne [PlatformID]::Win32NT -or -not [Environment]::Is64BitProcess) {
    throw '编译安装器需要 Windows 上的 64 位 PowerShell；其他平台可使用 -VerifyOnly。'
}

function Test-InnoCompiler([string] $Path) {
    if (-not $Path -or -not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $false }
    $compilerRoot = Split-Path -Parent $Path
    foreach ($name in @('ISCC.exe', 'ISCmplr.dll', 'ISPP.dll', 'ISPPBuiltins.iss', 'Setup.e32', 'SetupLdr.e32', 'Default.isl')) {
        if (-not (Test-Path -LiteralPath (Join-Path $compilerRoot $name) -PathType Leaf)) { Write-Verbose "Missing compiler file: $name"; return $false }
    }
    foreach ($binary in @($Path, (Join-Path $compilerRoot 'ISCmplr.dll'), (Join-Path $compilerRoot 'ISPP.dll'))) {
        $info = [Diagnostics.FileVersionInfo]::GetVersionInfo($binary)
        if ($info.FileMajorPart -ne 6 -or $info.FileMinorPart -ne 5 -or $info.FileBuildPart -ne 4) {
            Write-Verbose "Compiler version mismatch: $binary ($($info.FileVersion))"
            return $false
        }
    }
    return $true
}

$outRoot = Join-Path $windowsRoot 'out'
$releaseRoot = Join-Path $outRoot 'release'
$installerName = "XMJD-$version-windows-x64-setup.exe"
$installerPath = Join-Path $releaseRoot $installerName
if (Test-Path -LiteralPath $installerPath) { throw "安装器已存在，请先保留或显式移动它再重新打包：$installerPath" }
if ($IsccPath) {
    $IsccPath = (Resolve-Path -LiteralPath $IsccPath).Path
    if ([IO.Path]::GetFileName($IsccPath) -ine 'ISCC.exe' -or -not (Test-InnoCompiler $IsccPath)) {
        throw "IsccPath 必须指向完整的 Inno Setup $innoVersion 编译器目录中的 ISCC.exe。"
    }
} else {
    $candidates = @()
    $command = Get-Command 'ISCC.exe' -CommandType Application -ErrorAction SilentlyContinue
    if ($command) { $candidates += $command.Source }
    foreach ($parent in @(${env:ProgramFiles(x86)}, $env:ProgramFiles, $env:LOCALAPPDATA)) {
        if ($parent) {
            $relative = if ($parent -eq $env:LOCALAPPDATA) { 'Programs/Inno Setup 6/ISCC.exe' } else { 'Inno Setup 6/ISCC.exe' }
            $candidates += Join-Path $parent $relative
        }
    }
    foreach ($candidate in $candidates) {
        if (Test-InnoCompiler $candidate) { $IsccPath = (Resolve-Path -LiteralPath $candidate).Path; break }
    }
}

$toolStaging = $null
$releaseStaging = $null
try {
    if (-not $IsccPath) {
        # 只执行固定发布且 SHA-256 验证通过的引导程序，便携模式不注册文件关联或卸载项。
        $toolParent = Join-Path $outRoot 'tools'
        New-Item -ItemType Directory -Force -Path $toolParent | Out-Null
        $bootstrapPath = Join-Path $toolParent "innosetup-$innoVersion.exe"
        if (Test-Path -LiteralPath $bootstrapPath) {
            if ((Get-FileHash -LiteralPath $bootstrapPath -Algorithm SHA256).Hash.ToLowerInvariant() -ne $innoInstallerSha256) {
                throw "缓存的 Inno Setup 下载校验失败，未执行：$bootstrapPath"
            }
        } else {
            $downloadPath = Join-Path $toolParent ".innosetup-$([guid]::NewGuid().ToString('N')).download"
            try {
                [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
                Invoke-WebRequest -Uri $innoInstallerUrl -OutFile $downloadPath -UseBasicParsing
                if ((Get-FileHash -LiteralPath $downloadPath -Algorithm SHA256).Hash.ToLowerInvariant() -ne $innoInstallerSha256) {
                    throw 'Inno Setup 下载 SHA-256 与固定发布不一致，未执行下载内容。'
                }
                [IO.File]::Move($downloadPath, $bootstrapPath)
            } finally {
                if (Test-Path -LiteralPath $downloadPath) { Remove-Item -LiteralPath $downloadPath -Force }
            }
        }
        $toolStaging = Join-Path $toolParent ".inno-$([guid]::NewGuid().ToString('N'))"
        $bootstrapArguments = '/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /SP- /CURRENTUSER /PORTABLE=1 /TASKS="" /DIR="' + $toolStaging + '"'
        $bootstrap = Start-Process -FilePath $bootstrapPath -ArgumentList $bootstrapArguments -Wait -PassThru
        if ($bootstrap.ExitCode -ne 0) { throw "Inno Setup 便携解包失败，退出码：$($bootstrap.ExitCode)" }
        $IsccPath = Join-Path $toolStaging 'ISCC.exe'
        if (-not (Test-InnoCompiler $IsccPath)) {
            Get-ChildItem -LiteralPath $toolStaging -File | ForEach-Object {
                $versionInfo = [Diagnostics.FileVersionInfo]::GetVersionInfo($_.FullName)
                Write-Output "$($_.Name): $($versionInfo.FileVersion)"
            }
            throw "下载的工具未提供完整的 Inno Setup $innoVersion 编译器。"
        }
    }

    New-Item -ItemType Directory -Force -Path $releaseRoot | Out-Null
    $releaseStaging = Join-Path $releaseRoot ".staging-$([guid]::NewGuid().ToString('N'))"
    New-Item -ItemType Directory -Path $releaseStaging | Out-Null
    $installerScript = Join-Path $windowsRoot 'Installer/XMJD.iss'
    foreach ($path in @($packageRoot, $releaseStaging)) {
        if ($path -match '["\r\n{}]') { throw "路径不能含引号、换行或 Inno 常量分隔符：$path" }
    }
    $arguments = @('/Qp', "/DAppVersion=$version", "/DAppFileVersion=$fileVersion",
        "/DPackageDirectory=$packageRoot", "/DOutputDirectory=$releaseStaging", $installerScript)
    Write-Output "使用 Inno Setup $innoVersion：$IsccPath"
    & $IsccPath @arguments
    if ($LASTEXITCODE -ne 0) { throw "Inno Setup 编译失败，退出码：$LASTEXITCODE" }
    $compiledInstaller = Join-Path $releaseStaging $installerName
    if (-not (Test-Path -LiteralPath $compiledInstaller -PathType Leaf) -or (Get-Item -LiteralPath $compiledInstaller).Length -eq 0) {
        throw 'Inno Setup 未生成预期的非空安装器文件。'
    }
    # 发布前再次校验输入，防止编译期间交付文件发生变化。
    & (Join-Path $PSScriptRoot 'build.ps1') -VerifyPackage $packageRoot
    [IO.File]::Move($compiledInstaller, $installerPath)
} finally {
    if ($releaseStaging -and (Test-Path -LiteralPath $releaseStaging)) { Remove-Item -LiteralPath $releaseStaging -Recurse -Force }
    if ($toolStaging -and (Test-Path -LiteralPath $toolStaging)) { Remove-Item -LiteralPath $toolStaging -Recurse -Force }
}
Write-Output "安装器：$installerPath"
Write-Output "SHA-256：$((Get-FileHash -LiteralPath $installerPath -Algorithm SHA256).Hash.ToLowerInvariant())"
