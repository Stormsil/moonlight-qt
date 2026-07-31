[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $SourceRepository,

    [Parameter(Mandatory = $true)]
    [string] $Destination
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$contractPath = Join-Path $PSScriptRoot 'no-input-v1.contract.json'
$patchPath = Join-Path $PSScriptRoot 'no-input-v1.patch'
$contract = Get-Content -Raw -LiteralPath $contractPath | ConvertFrom-Json
if ($contract.schemaVersion -ne 1) {
    throw 'The Argus common-c patch contract version is unsupported.'
}

$source = (Resolve-Path -LiteralPath $SourceRepository).Path
$destinationPath = [IO.Path]::GetFullPath($Destination)
if (Test-Path -LiteralPath $destinationPath) {
    throw 'The Argus common-c materialization destination already exists.'
}

function Invoke-Git {
    param(
        [Parameter(Mandatory = $true)]
        [string] $WorkingDirectory,

        [Parameter(Mandatory = $true)]
        [string[]] $Arguments
    )

    $output = & git -C $WorkingDirectory @Arguments 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "Git failed with exit ${LASTEXITCODE}: $($output -join [Environment]::NewLine)"
    }

    return @($output | ForEach-Object { $_.ToString() })
}

function Get-MaterializedTreeSha256 {
    param(
        [Parameter(Mandatory = $true)]
        [string] $Root
    )

    $paths = [Collections.Generic.List[string]]::new()
    Get-ChildItem -LiteralPath $Root -Recurse -File -Force |
        ForEach-Object {
            $relative = [IO.Path]::GetRelativePath(
                $Root,
                $_.FullName).Replace('\', '/')
            if ($relative -notmatch '(^|/)\.git($|/)') {
                $paths.Add($relative)
            }
        }
    $ordered = $paths.ToArray()
    [Array]::Sort($ordered, [StringComparer]::Ordinal)
    $hash = [Security.Cryptography.IncrementalHash]::CreateHash(
        [Security.Cryptography.HashAlgorithmName]::SHA256)
    try {
        foreach ($relative in $ordered) {
            $fullPath = Join-Path $Root $relative.Replace('/', '\')
            $fileHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $fullPath).
                Hash.ToLowerInvariant()
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

$headOutput = @(Invoke-Git $source @('rev-parse', 'HEAD'))
$head = $headOutput[-1].Trim()
if ($head -cne $contract.baseCommit) {
    throw 'The common-c source repository is not at the pinned base commit.'
}
$status = @(Invoke-Git $source @(
    'status',
    '--porcelain=v1',
    '--untracked-files=all',
    '--ignore-submodules=none'))
if ($status.Count -ne 0) {
    throw 'The common-c source repository is not clean.'
}
$submoduleStatus = @(
    Invoke-Git $source @('submodule', 'status', '--recursive'))
$expectedSubmodules = @(
    " $($contract.submodules.enet) enet",
    " $($contract.submodules.nanors) nanors")
if ($submoduleStatus.Count -ne $expectedSubmodules.Count) {
    throw 'The common-c recursive submodule count does not match the contract.'
}
for ($index = 0; $index -lt $expectedSubmodules.Count; $index++) {
    if ($submoduleStatus[$index] -cnotlike "$($expectedSubmodules[$index])*" ) {
        throw 'The common-c recursive submodule pin does not match the contract.'
    }
}

$patchHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $patchPath).Hash
if ($patchHash -cne $contract.patchSha256) {
    throw 'The Argus common-c patch hash does not match the contract.'
}

$cloneOutput = & git -c core.autocrlf=false clone --no-hardlinks -- $source $destinationPath 2>&1
if ($LASTEXITCODE -ne 0) {
    throw "Git clone failed with exit ${LASTEXITCODE}: $($cloneOutput -join [Environment]::NewLine)"
}
Invoke-Git $destinationPath @('config', 'core.autocrlf', 'false') | Out-Null
Invoke-Git $destinationPath @('switch', '--detach', $contract.baseCommit) | Out-Null
Invoke-Git $destinationPath @('submodule', 'init') | Out-Null
Invoke-Git $destinationPath @(
    '-c',
    'core.autocrlf=false',
    '-c',
    'protocol.file.allow=always',
    '-c',
    "submodule.enet.url=$(Join-Path $source 'enet')",
    '-c',
    "submodule.nanors.url=$(Join-Path $source 'nanors')",
    'submodule',
    'update',
    '--recursive') | Out-Null

$baseTreeHash = Get-MaterializedTreeSha256 $destinationPath
if ($baseTreeHash -cne $contract.baseTreeSha256) {
    throw 'The materialized common-c base tree hash does not match the contract.'
}

$checkOutput = Invoke-Git $destinationPath @(
    'apply',
    '--check',
    '--whitespace=error-all',
    '--verbose',
    $patchPath)
if (($checkOutput -join "`n") -match '(?i)\b(offset|fuzz)\b') {
    throw 'The Argus common-c patch requires an offset or fuzz.'
}
$applyOutput = Invoke-Git $destinationPath @(
    'apply',
    '--whitespace=error-all',
    '--verbose',
    $patchPath)
if (($applyOutput -join "`n") -match '(?i)\b(offset|fuzz)\b') {
    throw 'The Argus common-c patch applied with an offset or fuzz.'
}

$resultTreeHash = Get-MaterializedTreeSha256 $destinationPath
if ($resultTreeHash -cne $contract.resultTreeSha256) {
    throw 'The patched common-c result tree hash does not match the contract.'
}

[ordered]@{
    schemaVersion = 1
    baseCommit = $head
    baseTreeSha256 = $baseTreeHash
    patchSha256 = $patchHash
    resultTreeSha256 = $resultTreeHash
    enetCommit = $contract.submodules.enet
    nanorsCommit = $contract.submodules.nanors
    patchAppliedWithoutFuzz = $true
    distributionApproved = $false
    legalApproval = $false
} | ConvertTo-Json
