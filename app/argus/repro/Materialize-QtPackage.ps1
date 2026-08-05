[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $UpdatesXmlPath,

    [Parameter(Mandatory = $true)]
    [string] $ArchiveDirectory,

    [Parameter(Mandatory = $true)]
    [string] $PackageObjectContractPath,

    [Parameter(Mandatory = $true)]
    [string] $PackageObjectReceiptPath,

    [Parameter(Mandatory = $true)]
    [string] $QtIdentityContractPath,

    [Parameter(Mandatory = $true)]
    [string] $PythonPackagesRoot,

    [Parameter(Mandatory = $true)]
    [string] $QtParentRoot,

    [Parameter(Mandatory = $true)]
    [string] $OutputReceiptPath
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Invoke-Python {
    param([string[]] $Arguments)

    & python @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "Python failed with exit code $LASTEXITCODE."
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
            "$(ConvertTo-Json $Value -Depth 10)`n",
            [Text.UTF8Encoding]::new($false))
        [IO.File]::Move($temporary, $fullPath, $true)
    }
    finally {
        if (Test-Path -LiteralPath $temporary) {
            Remove-Item -LiteralPath $temporary -Force
        }
    }
}

$scriptRoot = $PSScriptRoot
$objectReceiptTool = Join-Path $scriptRoot 'Write-QtPackageObjectReceipt.ps1'
$identityTool = Join-Path $scriptRoot 'Get-QtCanonicalIdentity.ps1'
$updatesXml = (Resolve-Path -LiteralPath $UpdatesXmlPath).Path
$archiveRoot = (Resolve-Path -LiteralPath $ArchiveDirectory).Path
$objectContract = (Resolve-Path -LiteralPath $PackageObjectContractPath).Path
$objectReceipt = (Resolve-Path -LiteralPath $PackageObjectReceiptPath).Path
$identityContract = (Resolve-Path -LiteralPath $QtIdentityContractPath).Path
$pythonPackages = (Resolve-Path -LiteralPath $PythonPackagesRoot).Path
$qtParent = [IO.Path]::GetFullPath($QtParentRoot)
$qtRoot = Join-Path $qtParent '6.11.1\msvc2022_64'
if (Test-Path -LiteralPath $qtParent) {
    throw "The Qt parent root already exists: '$qtParent'."
}

$temporaryReceipt = Join-Path ([IO.Path]::GetTempPath()) `
    "argus-qt-object-receipt-$([Guid]::NewGuid().ToString('N')).json"
$previousPythonPath = $env:PYTHONPATH
try {
    & $objectReceiptTool -UpdatesXmlPath $updatesXml `
        -ArchiveDirectory $archiveRoot -ContractPath $objectContract `
        -OutputPath $temporaryReceipt
    $expectedReceiptHash = (Get-FileHash -Algorithm SHA256 `
            -LiteralPath $objectReceipt).Hash
    $actualReceiptHash = (Get-FileHash -Algorithm SHA256 `
            -LiteralPath $temporaryReceipt).Hash
    if ($actualReceiptHash -cne $expectedReceiptHash) {
        throw 'The regenerated Qt package object receipt does not match the admitted receipt.'
    }

    $objectReceiptValue = Get-Content -Raw -LiteralPath $objectReceipt |
        ConvertFrom-Json
    [IO.Directory]::CreateDirectory($qtRoot) | Out-Null
    $env:PYTHONPATH = $pythonPackages
    $versions = & python -c `
        'import aqt, py7zr; print(aqt.__version__); print(py7zr.__version__)'
    if ($LASTEXITCODE -ne 0 -or
        $versions.Count -ne 2 -or
        $versions[0] -cne '3.3.0' -or
        $versions[1] -cne '1.1.3') {
        throw 'The Qt materializer requires task-local aqtinstall 3.3.0 and py7zr 1.1.3.'
    }

    foreach ($archive in $objectReceiptValue.archives) {
        $archivePath = Join-Path $archiveRoot $archive.objectName
        Invoke-Python @('-m', 'py7zr', 'x', $archivePath, $qtRoot)
    }
    $bin = Join-Path $qtRoot 'bin'
    foreach ($runtime in @('d3dcompiler_47.dll', 'opengl32sw.dll')) {
        $source = Join-Path $qtRoot $runtime
        if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
            throw "The expected Qt runtime '$runtime' was not extracted."
        }
        Move-Item -LiteralPath $source -Destination (Join-Path $bin $runtime)
    }

    $updaterCode = @'
import sys
from pathlib import Path
from aqt.archives import TargetConfig
from aqt.updater import Updater
Updater.update(TargetConfig("6.11.1", "desktop", "win64_msvc2022_64", "windows"), Path(sys.argv[1]), None)
'@
    Invoke-Python @('-c', $updaterCode, $qtParent)

    $qtIdentity = & $identityTool -QtRoot $qtRoot `
        -ContractPath $identityContract | ConvertFrom-Json
    & $objectReceiptTool -UpdatesXmlPath $updatesXml `
        -ArchiveDirectory $archiveRoot -ContractPath $objectContract `
        -OutputPath $temporaryReceipt
    $postExtractionReceiptHash = (Get-FileHash -Algorithm SHA256 `
            -LiteralPath $temporaryReceipt).Hash
    if ($postExtractionReceiptHash -cne $expectedReceiptHash) {
        throw 'A Qt package object changed while it was extracted.'
    }
    $materializationReceipt = [ordered]@{
        schemaVersion = 1
        packageObjectReceiptSha256 = $expectedReceiptHash
        archiveSetSha256 = $objectReceiptValue.archiveSetSha256
        qtIdentity = [ordered]@{
            schemaVersion = $qtIdentity.schemaVersion
            algorithm = $qtIdentity.algorithm
            contractSha256 = $qtIdentity.contractSha256
            canonicalTreeSha256 = $qtIdentity.canonicalTreeSha256
            excludedPaths = @($qtIdentity.excludedPaths)
            includedFileCount = $qtIdentity.includedFileCount
        }
        qtRootIdentitySha256 = [Convert]::ToHexString(
            [Security.Cryptography.SHA256]::HashData(
                [Text.Encoding]::UTF8.GetBytes(
                    ([IO.Path]::GetFullPath($qtRoot)).ToUpperInvariant())))
    }
    Write-AtomicJson $materializationReceipt $OutputReceiptPath
}
finally {
    $env:PYTHONPATH = $previousPythonPath
    if (Test-Path -LiteralPath $temporaryReceipt) {
        Remove-Item -LiteralPath $temporaryReceipt -Force
    }
}
