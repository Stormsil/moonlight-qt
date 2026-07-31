[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $SourceRoot,

    [Parameter(Mandatory = $true)]
    [string] $MaterializedCommonCRoot,

    [Parameter(Mandatory = $true)]
    [string] $WorkerPath,

    [Parameter(Mandatory = $true)]
    [string] $OutputPath,

    [Parameter(Mandatory = $true)]
    [ValidateSet('debug', 'release')]
    [string] $Configuration,

    [Parameter(Mandatory = $true)]
    [ValidateSet('x86', 'x64', 'arm64')]
    [string] $Architecture
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$source = (Resolve-Path -LiteralPath $SourceRoot).Path
$commonC = (Resolve-Path -LiteralPath $MaterializedCommonCRoot).Path
$worker = (Resolve-Path -LiteralPath $WorkerPath).Path
$output = [IO.Path]::GetFullPath($OutputPath)
$contractPath = Join-Path $source 'app\argus\common-c\no-input-v1.contract.json'
$patchPath = Join-Path $source 'app\argus\common-c\no-input-v1.patch'
$contract = Get-Content -Raw -LiteralPath $contractPath | ConvertFrom-Json

function Invoke-Git {
    param(
        [Parameter(Mandatory = $true)]
        [string] $WorkingDirectory,

        [Parameter(Mandatory = $true)]
        [string[]] $Arguments
    )

    $outputLines = & git -C $WorkingDirectory @Arguments 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "Git failed with exit ${LASTEXITCODE}: $($outputLines -join [Environment]::NewLine)"
    }
    return @($outputLines | ForEach-Object { $_.ToString() })
}

function Get-Sha256 {
    param(
        [Parameter(Mandatory = $true)]
        [string] $Path
    )

    $algorithm = [Security.Cryptography.SHA256]::Create()
    $stream = [IO.File]::OpenRead($Path)
    try {
        return [BitConverter]::ToString(
            $algorithm.ComputeHash($stream)).Replace('-', '')
    }
    finally {
        $stream.Dispose()
        $algorithm.Dispose()
    }
}

function Get-MaterializedTreeSha256 {
    param(
        [Parameter(Mandatory = $true)]
        [string] $Root
    )

    $paths = [Collections.Generic.List[string]]::new()
    $rootUri = [Uri]([IO.Path]::GetFullPath($Root).TrimEnd('\') + '\')
    Get-ChildItem -LiteralPath $Root -Recurse -File -Force |
        ForEach-Object {
            $relative = [Uri]::UnescapeDataString(
                $rootUri.MakeRelativeUri([Uri]$_.FullName).ToString())
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
            $fileHash = (Get-Sha256 $fullPath).ToLowerInvariant()
            $hash.AppendData([Text.Encoding]::UTF8.GetBytes($relative))
            $hash.AppendData([byte[]] 0)
            $hash.AppendData([Text.Encoding]::ASCII.GetBytes($fileHash))
            $hash.AppendData([byte[]] 10)
        }
        return [BitConverter]::ToString(
            $hash.GetHashAndReset()).Replace('-', '')
    }
    finally {
        $hash.Dispose()
    }
}

$repositoryRoot = @(Invoke-Git $source @('rev-parse', '--show-toplevel'))[-1].Trim()
if ([IO.Path]::GetFullPath($repositoryRoot) -cne [IO.Path]::GetFullPath($source)) {
    throw 'The build receipt source root is not the repository root.'
}
$forkCommit = @(Invoke-Git $source @('rev-parse', 'HEAD'))[-1].Trim()
$status = @(Invoke-Git $source @(
    'status',
    '--porcelain=v1',
    '--untracked-files=all',
    '--ignore-submodules=none'))
if ($status.Count -ne 0) {
    throw 'The Moonlight fork must be clean before producing a build receipt.'
}

$commonCHead = @(Invoke-Git $commonC @('rev-parse', 'HEAD'))[-1].Trim()
if ($commonCHead -cne $contract.baseCommit) {
    throw 'The materialized common-c base commit drifted.'
}
$patchSha256 = Get-Sha256 $patchPath
$resultTreeSha256 = Get-MaterializedTreeSha256 $commonC
if (($patchSha256 -cne $contract.patchSha256) -or
    ($resultTreeSha256 -cne $contract.resultTreeSha256)) {
    throw 'The materialized common-c source does not match the patch contract.'
}

$expectedWorker = Join-Path $source "build\deploy-$Architecture-$Configuration\Moonlight.exe"
if ([IO.Path]::GetFullPath($worker) -cne [IO.Path]::GetFullPath($expectedWorker)) {
    throw 'The build receipt worker path is not the canonical build output.'
}
$workerSha256 = Get-Sha256 $worker
$receipt = [ordered]@{
    schemaVersion = 1
    forkCommit = $forkCommit
    commonCBaseCommit = $contract.baseCommit
    patchSha256 = $patchSha256
    resultTreeSha256 = $resultTreeSha256
    configuration = $Configuration
    architecture = $Architecture
    buildCommand = "scripts\\build-arch.bat $Configuration $Architecture"
    commonCSourceAuthority = 'ARGUS_COMMON_C_DIR'
    workerFileName = 'Moonlight.exe'
    workerSha256 = $workerSha256
}

$parent = Split-Path -Parent $output
New-Item -ItemType Directory -Force -Path $parent | Out-Null
$temporary = "$output.$([Guid]::NewGuid().ToString('N')).tmp"
try {
    $json = $receipt | ConvertTo-Json
    [IO.File]::WriteAllText(
        $temporary,
        "$json`n",
        [Text.UTF8Encoding]::new($false))
    Move-Item -LiteralPath $temporary -Destination $output
}
finally {
    if (Test-Path -LiteralPath $temporary) {
        Remove-Item -LiteralPath $temporary -Force
    }
}
