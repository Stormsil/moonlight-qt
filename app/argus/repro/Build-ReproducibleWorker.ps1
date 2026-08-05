[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $BuildRoot,

    [Parameter(Mandatory = $true)]
    [string] $QtRoot,

    [string] $SourceRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Invoke-Git {
    param([string] $Root, [string[]] $Arguments)

    $lines = & git -C $Root @Arguments 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "Git failed with exit ${LASTEXITCODE}: $($lines -join [Environment]::NewLine)"
    }
    return @($lines | ForEach-Object { $_.ToString() })
}

function Get-Sha256 {
    param([string] $Path)

    return (Get-FileHash -Algorithm SHA256 -LiteralPath $Path).Hash
}

function Get-StringSha256 {
    param([string] $Value)

    $algorithm = [Security.Cryptography.SHA256]::Create()
    try {
        return [Convert]::ToHexString(
            $algorithm.ComputeHash([Text.Encoding]::UTF8.GetBytes($Value)))
    }
    finally {
        $algorithm.Dispose()
    }
}

function Get-TreeSha256 {
    param([string] $Root)

    $rootPath = (Resolve-Path -LiteralPath $Root).Path
    $paths = [Collections.Generic.List[string]]::new()
    Get-ChildItem -LiteralPath $rootPath -Recurse -File -Force |
        ForEach-Object {
            $paths.Add([IO.Path]::GetRelativePath(
                    $rootPath,
                    $_.FullName).Replace('\', '/'))
        }
    $ordered = $paths.ToArray()
    [Array]::Sort($ordered, [StringComparer]::Ordinal)
    $hash = [Security.Cryptography.IncrementalHash]::CreateHash(
        [Security.Cryptography.HashAlgorithmName]::SHA256)
    try {
        foreach ($relative in $ordered) {
            $fullPath = Join-Path $rootPath $relative.Replace('/', '\')
            $fileHash = (Get-Sha256 $fullPath).ToLowerInvariant()
            $hash.AppendData([Text.Encoding]::UTF8.GetBytes($relative))
            $hash.AppendData([byte[]] 0)
            $hash.AppendData([Text.Encoding]::ASCII.GetBytes($fileHash))
            $hash.AppendData([byte[]] 10)
        }
        return [Convert]::ToHexString($hash.GetHashAndReset())
    }
    finally {
        $hash.Dispose()
    }
}

function Assert-Hash {
    param([string] $Path, [string] $Expected, [string] $Name)

    $actual = Get-Sha256 $Path
    if ($actual -cne $Expected) {
        throw "$Name drifted: expected $Expected, got $actual."
    }
}

function Write-AtomicJson {
    param([object] $Value, [string] $Path)

    $temporary = "$Path.$([Guid]::NewGuid().ToString('N')).tmp"
    try {
        $json = $Value | ConvertTo-Json -Depth 12
        [IO.File]::WriteAllText(
            $temporary,
            "$json`n",
            [Text.UTF8Encoding]::new($false))
        Move-Item -LiteralPath $temporary -Destination $Path
    }
    finally {
        if (Test-Path -LiteralPath $temporary) {
            Remove-Item -LiteralPath $temporary -Force
        }
    }
}

$source = (Resolve-Path -LiteralPath $SourceRoot).Path
$qt = (Resolve-Path -LiteralPath $QtRoot).Path
$build = [IO.Path]::GetFullPath($BuildRoot)
if (-not [IO.Path]::IsPathFullyQualified($BuildRoot)) {
    throw 'BuildRoot must be an absolute path.'
}
if (Test-Path -LiteralPath $build) {
    throw 'BuildRoot must not exist; reproducible builds always start clean.'
}

$contractPath = Join-Path $PSScriptRoot 'reproducible-worker-v1.json'
$contract = Get-Content -Raw -LiteralPath $contractPath | ConvertFrom-Json
if ($contract.schemaVersion -ne 1 -or
    $contract.configuration -cne 'release' -or
    $contract.architecture -cne 'x64') {
    throw 'The reproducible worker contract is unsupported.'
}

$repositoryRoot = @(Invoke-Git $source @('rev-parse', '--show-toplevel'))[-1].Trim()
if ([IO.Path]::GetFullPath($repositoryRoot) -cne [IO.Path]::GetFullPath($source)) {
    throw 'SourceRoot must be the Moonlight repository root.'
}
$status = @(Invoke-Git $source @(
        'status',
        '--porcelain=v1',
        '--untracked-files=all',
        '--ignore-submodules=none'))
if ($status.Count -ne 0) {
    throw 'The Moonlight repository must be clean before a reproducible build.'
}
$autoCrlf = @(Invoke-Git $source @('config', '--get', 'core.autocrlf'))
if ($autoCrlf.Count -ne 1 -or $autoCrlf[0].Trim() -cne 'false') {
    throw 'core.autocrlf must be false for the canonical source checkout.'
}
$patchEol = @(Invoke-Git $source @(
        'ls-files',
        '--eol',
        'app/argus/common-c/no-input-v1.patch'))[-1]
if ($patchEol -cnotmatch 'i/lf\s+w/lf\s+attr/text eol=lf') {
    throw 'The no-input patch must be checked out with canonical LF bytes.'
}

& git -C $source merge-base --is-ancestor $contract.source.requiredAncestor HEAD
if ($LASTEXITCODE -ne 0) {
    throw 'The accepted Argus worker source is not an ancestor of HEAD.'
}
& git -C $source merge-base --is-ancestor $contract.source.upstreamCommit HEAD
if ($LASTEXITCODE -ne 0) {
    throw 'The pinned upstream Moonlight commit is not an ancestor of HEAD.'
}

$patchPath = Join-Path $source 'app\argus\common-c\no-input-v1.patch'
Assert-Hash $patchPath $contract.source.commonCPatchSha256 'common-c patch'

$qmake = Join-Path $qt 'bin\qmake.exe'
Assert-Hash $qmake $contract.qt.qmakeSha256 'qmake.exe'
$qtVersion = (& $qmake -query QT_VERSION).Trim()
if ($LASTEXITCODE -ne 0 -or $qtVersion -cne $contract.qt.version) {
    throw 'The Qt version drifted.'
}
$qtTreeSha256 = Get-TreeSha256 $qt
if ($qtTreeSha256 -cne $contract.qt.treeSha256) {
    throw 'The Qt input tree drifted.'
}

$dependencies = Join-Path $source 'libs\windows'
$dependencyTreeSha256 = Get-TreeSha256 $dependencies
if ($dependencyTreeSha256 -cne $contract.dependencies.treeSha256) {
    throw 'The official dependency input tree drifted.'
}

$jom = Join-Path $source 'scripts\jom.exe'
$vswhere = Join-Path $source 'scripts\vswhere.exe'
Assert-Hash $jom $contract.repositoryTools.'jom.exe' 'jom.exe'
Assert-Hash $vswhere $contract.repositoryTools.'vswhere.exe' 'vswhere.exe'
$vsInstall = (& $vswhere -latest -property installationPath).Trim()
$vsVersion = (& $vswhere -latest -property installationVersion).Trim()
if ($vsVersion -cne $contract.visualStudio.installationVersion) {
    throw 'The Visual Studio installation version drifted.'
}

$vcBin = Join-Path $vsInstall (
    "VC\Tools\MSVC\$($contract.visualStudio.vcToolsVersion)\bin\Hostx64\x64")
foreach ($toolName in $contract.visualStudio.tools.PSObject.Properties.Name) {
    Assert-Hash (Join-Path $vcBin $toolName) `
        $contract.visualStudio.tools.$toolName "MSVC $toolName"
}
$vcvars = Join-Path $vsInstall 'VC\Auxiliary\Build\vcvarsall.bat'

$sdkRoot = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10'
$sdkBin = Join-Path $sdkRoot "bin\$($contract.windowsSdk.version)\x64"
foreach ($toolName in $contract.windowsSdk.tools.PSObject.Properties.Name) {
    Assert-Hash (Join-Path $sdkBin $toolName) `
        $contract.windowsSdk.tools.$toolName "Windows SDK $toolName"
}

New-Item -ItemType Directory -Path $build | Out-Null
$objectRoot = Join-Path $build 'obj'
$tempRoot = Join-Path $build 'temp'
$artifactRoot = Join-Path $build 'artifact'
New-Item -ItemType Directory -Path $objectRoot, $tempRoot, $artifactRoot | Out-Null
$materializedCommonC = Join-Path $build 'common-c'
& (Join-Path $source 'app\argus\common-c\Materialize-CommonC.ps1') `
    -SourceRepository (Join-Path $source 'moonlight-common-c\moonlight-common-c') `
    -Destination $materializedCommonC | Out-Null

$canonicalSource = 'Z:\argus-source'
$canonicalBuild = 'Z:\argus-build'
$compilerFlags = "$($contract.environment.compilerFlags) " +
    "/pathmap:$source=$canonicalSource /pathmap:$build=$canonicalBuild"
$linkerFlags = $contract.environment.linkerFlags
$commandPath = Join-Path $build 'build.cmd'
$commandLines = @(
    '@echo off',
    'setlocal DisableDelayedExpansion',
    "call `"$vcvars`" amd64 >nul || exit /b 1",
    'set "CL="',
    'set "_CL_="',
    'set "LINK="',
    'set "_LINK_="',
    "set `"PATH=$(Join-Path $qt 'bin');%PATH%`"",
    "set `"TEMP=$tempRoot`"",
    "set `"TMP=$tempRoot`"",
    "set `"TZ=$($contract.environment.timezone)`"",
    "set `"VSLANG=$($contract.environment.visualStudioLanguage)`"",
    "set `"SOURCE_DATE_EPOCH=$($contract.environment.sourceDateEpoch)`"",
    "set `"CFLAGS=$compilerFlags`"",
    "set `"CXXFLAGS=$compilerFlags`"",
    "set `"LDFLAGS=$linkerFlags`"",
    "cd /d `"$objectRoot`" || exit /b 1",
    "`"$qmake`" -o Makefile `"$(Join-Path $source 'moonlight-qt.pro')`" `"ARGUS_COMMON_C_DIR=$materializedCommonC`" || exit /b 1",
    "`"$jom`" -f Makefile release || exit /b 1"
)
[IO.File]::WriteAllLines(
    $commandPath,
    $commandLines,
    [Text.ASCIIEncoding]::new())
$buildOutput = & cmd.exe /d /c $commandPath 2>&1
$buildOutput | Out-Host
if ($LASTEXITCODE -ne 0) {
    throw "The pinned Release x64 build failed with exit $LASTEXITCODE."
}

$workerSource = Join-Path $objectRoot 'app\release\Moonlight.exe'
$pdbSource = Join-Path $objectRoot 'app\release\Moonlight.pdb'
$worker = Join-Path $artifactRoot $contract.artifact.fileName
$pdb = Join-Path $artifactRoot $contract.artifact.pdbFileName
Copy-Item -LiteralPath $workerSource -Destination $worker
Copy-Item -LiteralPath $pdbSource -Destination $pdb

$dumpbin = Join-Path $vcBin 'dumpbin.exe'
$headers = & $dumpbin /headers $worker 2>&1
if ($LASTEXITCODE -ne 0 -or ($headers -join "`n") -cnotmatch '(?m)^\s*[0-9A-F]+\s+repro\s') {
    throw 'Moonlight.exe does not contain the required PE reproducibility entry.'
}
if (($headers -join "`n") -cnotmatch [Regex]::Escape(
        $contract.artifact.pdbAlternatePath)) {
    throw 'Moonlight.exe does not contain the canonical PDB alternate path.'
}
$workerText = [Text.Encoding]::ASCII.GetString(
    [IO.File]::ReadAllBytes($worker))
foreach ($forbiddenPath in @($source, $build, $qt, $materializedCommonC)) {
    if ($workerText.Contains($forbiddenPath, [StringComparison]::OrdinalIgnoreCase) -or
        $workerText.Contains(
            $forbiddenPath.Replace('\', '/'),
            [StringComparison]::OrdinalIgnoreCase)) {
        throw "Moonlight.exe embeds an absolute build input path: $forbiddenPath"
    }
}

$receipt = [ordered]@{
    schemaVersion = 1
    contractSha256 = Get-Sha256 $contractPath
    source = [ordered]@{
        forkCommit = @(Invoke-Git $source @('rev-parse', 'HEAD'))[-1].Trim()
        sourceTree = @(Invoke-Git $source @('rev-parse', 'HEAD^{tree}'))[-1].Trim()
        requiredAncestor = $contract.source.requiredAncestor
        upstreamCommit = $contract.source.upstreamCommit
        submodules = @(Invoke-Git $source @('submodule', 'status', '--recursive'))
        commonCBaseCommit = $contract.source.commonCBaseCommit
        commonCPatchSha256 = $contract.source.commonCPatchSha256
        commonCResultTreeSha256 = $contract.source.commonCResultTreeSha256
    }
    inputs = [ordered]@{
        qtVersion = $qtVersion
        qtTreeSha256 = $qtTreeSha256
        dependencyReleaseTag = $contract.dependencies.releaseTag
        dependencyTreeSha256 = $dependencyTreeSha256
        visualStudioVersion = $vsVersion
        vcToolsVersion = $contract.visualStudio.vcToolsVersion
        compilerVersion = $contract.visualStudio.compilerVersion
        windowsSdkVersion = $contract.windowsSdk.version
        repositoryTools = $contract.repositoryTools
    }
    environment = [ordered]@{
        sourceDateEpoch = $contract.environment.sourceDateEpoch
        sourceDateEpochHonoredByMsvc = $false
        timezone = $contract.environment.timezone
        visualStudioLanguage = $contract.environment.visualStudioLanguage
        compilerFlags = $compilerFlags
        linkerFlags = $linkerFlags
        canonicalSourcePath = $canonicalSource
        canonicalBuildPath = $canonicalBuild
    }
    buildRootIdentitySha256 = Get-StringSha256 $build.ToUpperInvariant()
    artifact = [ordered]@{
        fileName = $contract.artifact.fileName
        sha256 = Get-Sha256 $worker
        length = (Get-Item -LiteralPath $worker).Length
        pdbFileName = $contract.artifact.pdbFileName
        pdbSha256 = Get-Sha256 $pdb
        pdbAlternatePath = $contract.artifact.pdbAlternatePath
        peReproDebugEntry = $true
        embeddedAbsoluteInputPaths = $false
    }
}
$receiptPath = Join-Path $artifactRoot 'reproducible-worker-receipt.json'
Write-AtomicJson $receipt $receiptPath
$receipt | ConvertTo-Json -Depth 12
