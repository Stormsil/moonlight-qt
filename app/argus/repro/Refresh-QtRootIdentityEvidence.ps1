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
    [Parameter(Mandatory = $true)] [string] $ConsumptionEvidencePath
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$workerSha256 =
    'C0CACE23418F88F8DABD53EB1B4810C68FE10A4C055400D8BA59360B15B92385'
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
        [IO.File]::WriteAllText(
            $temporary,
            "$(ConvertTo-Json $Value -Depth 14)`n",
            [Text.UTF8Encoding]::new($false))
        [IO.File]::Move($temporary, $fullPath, $true)
    }
    finally {
        if (Test-Path -LiteralPath $temporary) {
            Remove-Item -LiteralPath $temporary -Force
        }
    }
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
        $receipt.qtIdentity.schemaVersion -ne 1 -or
        $receipt.qtIdentity.algorithm -cne
            'sha256-relative-path-nul-content-sha256-lf-v1' -or
        $receipt.qtIdentity.canonicalTreeSha256 -cne $canonicalQtSha256 -or
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
$atomicReceipts = @()
for ($index = 0; $index -lt 2; $index++) {
    $atomic = Get-Content -Raw -LiteralPath $atomicReceiptPaths[$index] |
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
    Set-JsonProperty $atomic 'qtRootIdentitySha256' `
        $rootIdentities[$index]
    Write-AtomicJson $atomic $atomicReceiptPaths[$index]
    $atomicReceipts += $atomic
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
$proof.buildReceipts = @($atomicReceiptPaths | ForEach-Object {
        $bytes = [IO.File]::ReadAllBytes($_)
        [ordered]@{
            sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $_).Hash
            utf8Base64 = [Convert]::ToBase64String($bytes)
        }
    })
Write-AtomicJson $proof $ProofPath

Write-AtomicJson $materializations[2] $AdmissionEvidencePath
$consumption = Get-Content -Raw -LiteralPath `
    (Resolve-Path -LiteralPath $ConsumptionEvidencePath).Path |
    ConvertFrom-Json
if ($consumption.schemaVersion -ne 1 -or
    $consumption.threeRootIdentity.canonicalTreeSha256 -cne
        $canonicalQtSha256) {
    throw 'The Qt build-consumption receipt is not the admitted evidence.'
}
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
