[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $BuildRootA,

    [Parameter(Mandatory = $true)]
    [string] $BuildRootB,

    [Parameter(Mandatory = $true)]
    [string] $QtRoot,

    [string] $SourceRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path,

    [Parameter(Mandatory = $true)]
    [string] $ProofPath,

    [switch] $UseExistingBuilds
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Write-AtomicJson {
    param([object] $Value, [string] $Path)

    $parent = Split-Path -Parent ([IO.Path]::GetFullPath($Path))
    New-Item -ItemType Directory -Force -Path $parent | Out-Null
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

$rootA = [IO.Path]::GetFullPath($BuildRootA)
$rootB = [IO.Path]::GetFullPath($BuildRootB)
if ($rootA -ceq $rootB) {
    throw 'The two reproducibility build roots must be different.'
}

$buildScript = Join-Path $PSScriptRoot 'Build-ReproducibleWorker.ps1'
if ($UseExistingBuilds) {
    $receiptA = Get-Content -Raw -LiteralPath (Join-Path $rootA `
        'artifact\reproducible-worker-receipt.json') | ConvertFrom-Json
    $receiptB = Get-Content -Raw -LiteralPath (Join-Path $rootB `
        'artifact\reproducible-worker-receipt.json') | ConvertFrom-Json
}
else {
    $receiptA = & $buildScript -BuildRoot $rootA -QtRoot $QtRoot `
        -SourceRoot $SourceRoot | ConvertFrom-Json
    $receiptB = & $buildScript -BuildRoot $rootB -QtRoot $QtRoot `
        -SourceRoot $SourceRoot | ConvertFrom-Json
}

$workerA = Join-Path $rootA 'artifact\Moonlight.exe'
$workerB = Join-Path $rootB 'artifact\Moonlight.exe'
$bytesA = [IO.File]::ReadAllBytes($workerA)
$bytesB = [IO.File]::ReadAllBytes($workerB)
if (-not [Linq.Enumerable]::SequenceEqual[byte]($bytesA, $bytesB)) {
    throw 'The two clean Moonlight.exe builds are not byte-for-byte identical.'
}
if ((Get-FileHash -Algorithm SHA256 -LiteralPath $workerA).Hash -cne
        $receiptA.artifact.sha256 -or
    (Get-FileHash -Algorithm SHA256 -LiteralPath $workerB).Hash -cne
        $receiptB.artifact.sha256) {
    throw 'A worker artifact does not match its atomic build receipt.'
}
if ($receiptA.contractSha256 -cne $receiptB.contractSha256 -or
    $receiptA.source.forkCommit -cne $receiptB.source.forkCommit -or
    $receiptA.source.sourceTree -cne $receiptB.source.sourceTree -or
    $receiptA.inputs.qtTreeSha256 -cne $receiptB.inputs.qtTreeSha256 -or
    $receiptA.inputs.dependencyTreeSha256 -cne
        $receiptB.inputs.dependencyTreeSha256) {
    throw 'The two builds did not use the same pinned source and input contract.'
}
if ($receiptA.buildRootIdentitySha256 -ceq
    $receiptB.buildRootIdentitySha256) {
    throw 'The two build-root identities unexpectedly match.'
}

$pdbA = Join-Path $rootA 'artifact\Moonlight.pdb'
$pdbB = Join-Path $rootB 'artifact\Moonlight.pdb'
$pdbTextA = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($pdbA))
$pdbTextB = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($pdbB))
$pdbContainsTaskLocalPaths =
    $pdbTextA.Contains($rootA, [StringComparison]::OrdinalIgnoreCase) -and
    $pdbTextB.Contains($rootB, [StringComparison]::OrdinalIgnoreCase)
$pdbContentIdentical = $receiptA.artifact.pdbSha256 -ceq
    $receiptB.artifact.pdbSha256

$proof = [ordered]@{
    schemaVersion = 1
    sourceCommit = $receiptA.source.forkCommit
    sourceTree = $receiptA.source.sourceTree
    contractSha256 = $receiptA.contractSha256
    absoluteRootsDistinct = $true
    buildRootIdentitySha256 = @(
        $receiptA.buildRootIdentitySha256,
        $receiptB.buildRootIdentitySha256)
    worker = [ordered]@{
        fileName = 'Moonlight.exe'
        sha256 = $receiptA.artifact.sha256
        length = $receiptA.artifact.length
        byteForByteIdentical = $true
        peReproDebugEntry = $true
        embeddedAbsoluteInputPaths = $false
    }
    pdb = [ordered]@{
        fileName = 'Moonlight.pdb'
        sha256 = @(
            $receiptA.artifact.pdbSha256,
            $receiptB.artifact.pdbSha256)
        contentIdentical = $pdbContentIdentical
        embeddedPath = $receiptA.artifact.pdbAlternatePath
        taskLocalPathsPresent = $pdbContainsTaskLocalPaths
        canonicalArtifact = $false
        distributed = $false
    }
    inputs = $receiptA.inputs
    source = $receiptA.source
    environment = $receiptA.environment
}
Write-AtomicJson $proof $ProofPath
$proof | ConvertTo-Json -Depth 12
