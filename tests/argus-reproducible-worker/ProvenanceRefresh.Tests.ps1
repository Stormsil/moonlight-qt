[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$sourceRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$refreshScript = Join-Path $sourceRoot `
    'app\argus\repro\Refresh-QtRootIdentityEvidence.ps1'
$source = [IO.File]::ReadAllText($refreshScript)

foreach ($historicalLiteral in @(
        '7C1679E451DE24FDB570FBA348450A4175876E0CEC77CBB7A91DBFD2F196C716',
        'fix/issue-979-renderer-free-worker')) {
    if ($source.Contains($historicalLiteral, [StringComparison]::Ordinal)) {
        throw "The provenance refresh remains bound to historical input '$historicalLiteral'."
    }
}

if (-not $source.Contains('[string] $SourceBranch',
        [StringComparison]::Ordinal) -or
    -not $source.Contains('$originalProof.worker.sha256',
        [StringComparison]::Ordinal) -or
    -not $source.Contains('$originalProof.inputs.qt.identity.canonicalTreeSha256',
        [StringComparison]::Ordinal)) {
    throw 'The provenance refresh does not derive task identity from its admitted proof and branch.'
}

Write-Output 'Provenance refresh contract tests passed.'
