[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [string] $BuildRootA,
    [Parameter(Mandatory = $true)] [string] $BuildRootB,
    [Parameter(Mandatory = $true)] [string] $QtRootA,
    [Parameter(Mandatory = $true)] [string] $QtRootB,
    [Parameter(Mandatory = $true)] [string] $AdmissionQtRoot,
    [Parameter(Mandatory = $true)] [string] $QtMaterializationReceiptA,
    [Parameter(Mandatory = $true)] [string] $QtMaterializationReceiptB,
    [Parameter(Mandatory = $true)] [string] $AdmissionMaterializationReceipt,
    [Parameter(Mandatory = $true)] [string] $ProofPath,
    [Parameter(Mandatory = $true)] [string] $AdmissionEvidencePath,
    [Parameter(Mandatory = $true)] [string] $ConsumptionEvidencePath,
    [Parameter(Mandatory = $true)] [string] $UpdatesXmlPath,
    [Parameter(Mandatory = $true)] [string] $ArchiveDirectory,
    [Parameter(Mandatory = $true)] [string] $PackageObjectContractPath,
    [Parameter(Mandatory = $true)] [string] $PackageObjectReceiptPath,
    [Parameter(Mandatory = $true)] [string] $IdentityContractPath,
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[0-9a-f]{40}$')]
    [string] $BuildLogicCommitAuthority,
    [Parameter(Mandatory = $true)] [string] $BotRoot
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$workerSha256 =
    '6A6F8A499797F5A46B0601D0CECD391C96C7FF45B5F13289FF07B26D58FF4D8D'
$canonicalQtSha256 =
    'B8272265B99CCEE3227C4B01E12517482DC913FC91A23A3BB609961FF3696FD8'
$packageReceiptSha256 =
    'BA123F16C09445C65B4A2D06B976D9FB440D978D420F09D6EB836FE5D1C11C48'
$archiveSetSha256 =
    '5E2401AB07D6F2B63D8D2FC9991E261840D088A6BD0FD5414649CB9578165C02'

function Get-PathIdentity {
    param([string] $Path)

    $fullPath = (Resolve-Path -LiteralPath $Path).Path
    return [Convert]::ToHexString(
        [Security.Cryptography.SHA256]::HashData(
            [Text.Encoding]::UTF8.GetBytes($fullPath.ToUpperInvariant())))
}

function Set-JsonProperty {
    param([object] $Value, [string] $Name, [object] $PropertyValue)

    if ($null -eq $Value.PSObject.Properties[$Name]) {
        $Value | Add-Member -NotePropertyName $Name `
            -NotePropertyValue $PropertyValue
    }
    else {
        $Value.$Name = $PropertyValue
    }
}

function Write-AtomicJson {
    param([object] $Value, [string] $Path)

    $fullPath = [IO.Path]::GetFullPath($Path)
    $parent = Split-Path -Parent $fullPath
    [IO.Directory]::CreateDirectory($parent) | Out-Null
    $temporary = Join-Path $parent `
        ".$([IO.Path]::GetFileName($fullPath)).$([Guid]::NewGuid().ToString('N')).tmp"
    try {
        $json = (ConvertTo-Json $Value -Depth 14).Replace("`r`n", "`n")
        [IO.File]::WriteAllText(
            $temporary,
            "$json`n",
            [Text.UTF8Encoding]::new($false))
        [IO.File]::Move($temporary, $fullPath, $true)
    }
    finally {
        if (Test-Path -LiteralPath $temporary) {
            Remove-Item -LiteralPath $temporary -Force
        }
    }
}

function Get-CrLfFileSha256 {
    param([string] $Path)

    $text = [IO.File]::ReadAllText((Resolve-Path -LiteralPath $Path).Path)
    $canonical = $text.Replace("`r`n", "`n").Replace("`n", "`r`n")
    return [Convert]::ToHexString(
        [Security.Cryptography.SHA256]::HashData(
            [Text.UTF8Encoding]::new($false).GetBytes($canonical)))
}

$qtRoots = @(
    (Resolve-Path -LiteralPath $QtRootA).Path,
    (Resolve-Path -LiteralPath $QtRootB).Path,
    (Resolve-Path -LiteralPath $AdmissionQtRoot).Path)
$uniqueRoots = [Collections.Generic.HashSet[string]]::new(
    [StringComparer]::OrdinalIgnoreCase)
foreach ($root in $qtRoots) {
    if (-not $uniqueRoots.Add($root)) {
        throw 'Build and admission Qt roots must be three distinct paths.'
    }
}
$rootIdentities = @($qtRoots | ForEach-Object { Get-PathIdentity $_ })
if (($rootIdentities | Select-Object -Unique).Count -ne 3) {
    throw 'Build and admission Qt root identities must be distinct.'
}

$temporaryObjectReceipt = Join-Path ([IO.Path]::GetTempPath()) `
    "argus-qt-post-consumption-$([Guid]::NewGuid().ToString('N')).json"
try {
    & (Join-Path $PSScriptRoot 'Write-QtPackageObjectReceipt.ps1') `
        -UpdatesXmlPath $UpdatesXmlPath `
        -ArchiveDirectory $ArchiveDirectory `
        -ContractPath $PackageObjectContractPath `
        -OutputPath $temporaryObjectReceipt
    if ((Get-FileHash -Algorithm SHA256 -LiteralPath `
            $temporaryObjectReceipt).Hash -cne $packageReceiptSha256 -or
        (Get-FileHash -Algorithm SHA256 -LiteralPath `
            $PackageObjectReceiptPath).Hash -cne $packageReceiptSha256) {
        throw 'The exact eight-object Qt package receipt did not revalidate.'
    }
}
finally {
    if (Test-Path -LiteralPath $temporaryObjectReceipt) {
        Remove-Item -LiteralPath $temporaryObjectReceipt -Force
    }
}

$identityTool = Join-Path $PSScriptRoot 'Get-QtCanonicalIdentity.ps1'
$actualIdentities = @($qtRoots | ForEach-Object {
        & $identityTool -QtRoot $_ -ContractPath $IdentityContractPath |
            ConvertFrom-Json
    })
foreach ($identity in $actualIdentities) {
    if ($identity.schemaVersion -ne 1 -or
        $identity.algorithm -cne
            'sha256-relative-path-nul-content-sha256-lf-v1' -or
        $identity.canonicalTreeSha256 -cne $canonicalQtSha256 -or
        $identity.excludedPaths.Count -ne 1 -or
        $identity.excludedPaths[0] -cne 'bin/qtenv2.bat') {
        throw 'A preserved Qt root failed post-consumption revalidation.'
    }
}

$materializationPaths = @(
    $QtMaterializationReceiptA,
    $QtMaterializationReceiptB,
    $AdmissionMaterializationReceipt)
$materializations = @()
for ($index = 0; $index -lt 3; $index++) {
    $receipt = Get-Content -Raw -LiteralPath `
        (Resolve-Path -LiteralPath $materializationPaths[$index]).Path |
        ConvertFrom-Json
    if ($receipt.schemaVersion -ne 1 -or
        $receipt.packageObjectReceiptSha256 -cne $packageReceiptSha256 -or
        $receipt.archiveSetSha256 -cne $archiveSetSha256 -or
        $receipt.qtIdentity.schemaVersion -ne
            $actualIdentities[$index].schemaVersion -or
        $receipt.qtIdentity.algorithm -cne
            $actualIdentities[$index].algorithm -or
        $receipt.qtIdentity.canonicalTreeSha256 -cne
            $actualIdentities[$index].canonicalTreeSha256 -or
        $receipt.qtIdentity.excludedPaths.Count -ne 1 -or
        $receipt.qtIdentity.excludedPaths[0] -cne 'bin/qtenv2.bat') {
        throw 'A Qt materialization receipt is not the admitted exact input.'
    }
    Set-JsonProperty $receipt 'qtRootIdentitySha256' `
        $rootIdentities[$index]
    Write-AtomicJson $receipt $materializationPaths[$index]
    $materializations += $receipt
}

$buildRoots = @(
    (Resolve-Path -LiteralPath $BuildRootA).Path,
    (Resolve-Path -LiteralPath $BuildRootB).Path)
$atomicReceiptPaths = @($buildRoots | ForEach-Object {
        Join-Path $_ 'artifact\reproducible-worker-receipt.json'
    })
$originalProof = Get-Content -Raw -LiteralPath `
    (Resolve-Path -LiteralPath $ProofPath) | ConvertFrom-Json
if ($originalProof.schemaVersion -ne 2 -or
    $originalProof.worker.sha256 -cne $workerSha256 -or
    $originalProof.worker.byteForByteIdentical -ne $true -or
    $originalProof.buildReceipts.Count -ne 2) {
    throw 'The current atomic proof is invalid.'
}
for ($index = 0; $index -lt 2; $index++) {
    $originalBytes = [Convert]::FromBase64String(
        $originalProof.buildReceipts[$index].utf8Base64)
    if ([Convert]::ToHexString(
            [Security.Cryptography.SHA256]::HashData($originalBytes)) -cne
        $originalProof.buildReceipts[$index].sha256) {
        throw 'An atomic build receipt hash is invalid.'
    }
    [IO.File]::WriteAllBytes($atomicReceiptPaths[$index], $originalBytes)
    $atomic = [Text.Encoding]::UTF8.GetString($originalBytes) |
        ConvertFrom-Json
    $worker = Join-Path $buildRoots[$index] 'artifact\Moonlight.exe'
    if ($atomic.schemaVersion -ne 2 -or
        $atomic.artifact.sha256 -cne $workerSha256 -or
        (Get-FileHash -Algorithm SHA256 -LiteralPath $worker).Hash -cne
            $workerSha256 -or
        $atomic.inputs.qt.identity.canonicalTreeSha256 -cne
            $canonicalQtSha256 -or
        $atomic.inputs.qt.packageObjects.receiptSha256 -cne
            $packageReceiptSha256) {
        throw 'An atomic build receipt is not bound to the admitted worker.'
    }
}

$proof = Get-Content -Raw -LiteralPath (Resolve-Path -LiteralPath $ProofPath) |
    ConvertFrom-Json
if ($proof.schemaVersion -ne 2 -or
    $proof.worker.sha256 -cne $workerSha256 -or
    $proof.worker.byteForByteIdentical -ne $true) {
    throw 'The two-build proof is not the admitted exact worker proof.'
}
Set-JsonProperty $proof 'absoluteQtRootsDistinct' $true
Set-JsonProperty $proof 'qtRootIdentitySha256' @(
    $rootIdentities[0],
    $rootIdentities[1])
$proof.buildReceipts = @($originalProof.buildReceipts)
Set-JsonProperty $proof 'qtRootIdentityAttestation' ([ordered]@{
        schemaVersion = 1
        timing = 'post-consumption-existing-artifact'
        algorithm = 'sha256-uppercase-normalized-absolute-path-utf8-v1'
        qtRootIdentitySha256 = $rootIdentities
        canonicalIdentityRevalidated = $true
        packageObjectsRevalidated = $true
        packageObjectReceiptSha256 = $packageReceiptSha256
        canonicalTreeSha256 = $canonicalQtSha256
    })
Write-AtomicJson $proof $ProofPath

$sourceInventoryPath = Join-Path $PSScriptRoot `
    '..\package\source-inventory.json'
$correspondingSourcePath = Join-Path $PSScriptRoot `
    '..\package\corresponding-source.json'
$sourceInventory = Get-Content -Raw -LiteralPath $sourceInventoryPath |
    ConvertFrom-Json
$correspondingSource = Get-Content -Raw -LiteralPath `
    $correspondingSourcePath | ConvertFrom-Json
if ($sourceInventory.schemaVersion -ne 4 -or
    $correspondingSource.schemaVersion -ne 5) {
    throw 'The corresponding-source receipts are not the admitted schemas.'
}
Set-JsonProperty $sourceInventory.qtInput 'rootIdentityAlgorithm' `
    'sha256-uppercase-normalized-absolute-path-utf8-v1'
Set-JsonProperty $sourceInventory 'sourceBranch' `
    'fix/issue-979-renderer-free-worker'
Set-JsonProperty $sourceInventory 'artifactSourceCommit' `
    $proof.source.forkCommit
Set-JsonProperty $sourceInventory 'artifactSourceTree' `
    $proof.source.sourceTree
Set-JsonProperty $sourceInventory 'artifactSha256' $workerSha256
Set-JsonProperty $correspondingSource 'sourceBranch' `
    'fix/issue-979-renderer-free-worker'
Set-JsonProperty $correspondingSource 'artifactSourceCommit' `
    $proof.source.forkCommit
Set-JsonProperty $correspondingSource 'artifactSourceTree' `
    $proof.source.sourceTree
Set-JsonProperty $correspondingSource 'buildLogicCommitAuthority' `
    $BuildLogicCommitAuthority
Set-JsonProperty $correspondingSource.reproducibleBuild `
    'qtRootIdentitySha256' @($rootIdentities[0], $rootIdentities[1])
Set-JsonProperty $correspondingSource.reproducibleBuild `
    'rootIdentityRefreshScript' `
    'app/argus/repro/Refresh-QtRootIdentityEvidence.ps1'
$correspondingSource.reproducibleBuild.twoBuildProofSha256 =
    (Get-FileHash -Algorithm SHA256 -LiteralPath $ProofPath).Hash
$correspondingSource.reproducibleBuild.contractSha256 =
    $proof.contractSha256
$correspondingSource.reproducibleBuild.workerSha256 = $workerSha256
$correspondingSource.reproducibleBuild.buildReceiptSha256 = @(
    $atomicReceiptPaths | ForEach-Object {
        (Get-FileHash -Algorithm SHA256 -LiteralPath $_).Hash
    })
Write-AtomicJson $sourceInventory $sourceInventoryPath
Write-AtomicJson $correspondingSource $correspondingSourcePath

Write-AtomicJson $materializations[2] $AdmissionEvidencePath
$consumption = Get-Content -Raw -LiteralPath `
    (Resolve-Path -LiteralPath $ConsumptionEvidencePath).Path |
    ConvertFrom-Json
if ($consumption.schemaVersion -ne 1 -or
    $consumption.threeRootIdentity.canonicalTreeSha256 -cne
        $canonicalQtSha256) {
    throw 'The Qt build-consumption receipt is not the admitted evidence.'
}
$sourceRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$consumption.callGraph.entryPointSha256 =
    (Get-FileHash -Algorithm SHA256 -LiteralPath `
        (Join-Path $sourceRoot `
            'app\argus\repro\Verify-ReproducibleWorker.ps1')).Hash
$consumption.callGraph.buildScriptSha256 =
    (Get-FileHash -Algorithm SHA256 -LiteralPath `
        (Join-Path $sourceRoot `
            'app\argus\repro\Build-ReproducibleWorker.ps1')).Hash
$consumption.callGraph.repositoryBuildArchScriptSha256 =
    Get-CrLfFileSha256 (Join-Path $sourceRoot 'scripts\build-arch.bat')
Set-JsonProperty $consumption.threeRootIdentity `
    'qtRootIdentitySha256' $rootIdentities
$consumption.threeRootIdentity.materializationReceiptSha256 =
    (Get-FileHash -Algorithm SHA256 -LiteralPath `
        $AdmissionMaterializationReceipt).Hash
Write-AtomicJson $consumption $ConsumptionEvidencePath

[ordered]@{
    schemaVersion = 1
    qtRootIdentitySha256 = $rootIdentities
    proofSha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $ProofPath).Hash
    buildReceiptSha256 = @($atomicReceiptPaths | ForEach-Object {
            (Get-FileHash -Algorithm SHA256 -LiteralPath $_).Hash
        })
    admissionReceiptSha256 = (Get-FileHash -Algorithm SHA256 `
        -LiteralPath $AdmissionEvidencePath).Hash
} | ConvertTo-Json -Depth 5
