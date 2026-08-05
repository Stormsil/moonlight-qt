[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Assert-Equal {
    param(
        [object] $Expected,
        [object] $Actual,
        [string] $Message)

    if ($Expected -cne $Actual) {
        throw "$Message Expected '$Expected', got '$Actual'."
    }
}

function Assert-NotEqual {
    param(
        [object] $Unexpected,
        [object] $Actual,
        [string] $Message)

    if ($Unexpected -ceq $Actual) {
        throw "$Message Both values were '$Actual'."
    }
}

function Assert-Rejected {
    param(
        [scriptblock] $Action,
        [string] $MessagePattern)

    try {
        & $Action
    }
    catch {
        if ($_.Exception.Message -notlike $MessagePattern) {
            throw "Expected rejection '$MessagePattern', got '$($_.Exception.Message)'."
        }
        return
    }
    throw "Expected rejection '$MessagePattern', but the command succeeded."
}

function Write-Utf8File {
    param(
        [string] $Path,
        [string] $Value)

    $parent = Split-Path -Parent $Path
    [IO.Directory]::CreateDirectory($parent) | Out-Null
    [IO.File]::WriteAllText(
        $Path,
        $Value,
        [Text.UTF8Encoding]::new($false))
}

function Write-Contract {
    param(
        [string] $Path,
        [int] $SchemaVersion = 1,
        [object[]] $ExcludedPaths = @(
            [ordered]@{
                path = 'bin/qtenv2.bat'
                classification = 'qt-generated-environment-launcher'
                buildConsumption = 'proven-unused-release-x64'
                evidence = 'call-graph-and-file-io-v1'
            }),
        [switch] $IncludeUnknownProperty)

    $contract = [ordered]@{
        schemaVersion = $SchemaVersion
        algorithm = 'sha256-relative-path-nul-content-sha256-lf-v1'
        pathComparison = 'ordinal'
        excludedPaths = $ExcludedPaths
    }
    if ($IncludeUnknownProperty) {
        $contract.unknown = $true
    }
    $json = $contract | ConvertTo-Json -Depth 8
    Write-Utf8File $Path "$json`n"
}

function New-QtFixture {
    param(
        [string] $Root,
        [string] $EnvironmentRoot)

    Write-Utf8File (Join-Path $Root 'bin\qmake.exe') 'qmake-consumed'
    Write-Utf8File (Join-Path $Root 'lib\Qt6Core.lib') 'library-consumed'
    Write-Utf8File (Join-Path $Root 'plugins\imageformats\qjpeg.dll') `
        'plugin-consumed'
    Write-Utf8File (Join-Path $Root 'bin\qtenv2.bat') `
        "set QT_ROOT=$EnvironmentRoot`r`n"
}

$sourceRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$identityTool = Join-Path $sourceRoot `
    'app\argus\repro\Get-QtCanonicalIdentity.ps1'
$temporaryRoot = Join-Path ([IO.Path]::GetTempPath()) `
    "argus-qt-identity-$([Guid]::NewGuid().ToString('N'))"

try {
    $qtA = Join-Path $temporaryRoot 'root-a\Qt\6.11.1\msvc2022_64'
    $qtB = Join-Path $temporaryRoot 'root-b\Qt\6.11.1\msvc2022_64'
    $contractPath = Join-Path $sourceRoot `
        'app\argus\repro\qt-canonical-identity-v1.json'
    New-QtFixture $qtA 'C:\task-a\Qt\6.11.1\msvc2022_64'
    New-QtFixture $qtB 'D:\different-task\Qt\6.11.1\msvc2022_64'
    $identityA = & $identityTool -QtRoot $qtA -ContractPath $contractPath |
        ConvertFrom-Json
    $identityB = & $identityTool -QtRoot $qtB -ContractPath $contractPath |
        ConvertFrom-Json

    Assert-Equal 1 $identityA.schemaVersion 'Identity schema drifted.'
    Assert-Equal $identityA.canonicalTreeSha256 `
        $identityB.canonicalTreeSha256 `
        'Task-local qtenv2.bat content changed the canonical Qt identity.'
    Assert-Equal 1 $identityA.excludedPaths.Count `
        'The identity did not report exactly one exclusion.'
    Assert-Equal 'bin/qtenv2.bat' $identityA.excludedPaths[0] `
        'The identity excluded an unexpected path.'

    foreach ($consumedInput in @(
            [ordered]@{ Path = 'bin\qmake.exe'; Value = 'changed-qmake'; Kind = 'tool' },
            [ordered]@{ Path = 'lib\Qt6Core.lib'; Value = 'changed-library'; Kind = 'library' },
            [ordered]@{ Path = 'plugins\imageformats\qjpeg.dll'; Value = 'changed-plugin'; Kind = 'plugin' },
            [ordered]@{ Path = 'unknown\extra.bin'; Value = 'new-input'; Kind = 'additional file' })) {
        $changedRoot = Join-Path $temporaryRoot "changed-$($consumedInput.Kind.Replace(' ', '-'))"
        New-QtFixture $changedRoot 'E:\another-task\Qt\6.11.1\msvc2022_64'
        Write-Utf8File (Join-Path $changedRoot $consumedInput.Path) `
            $consumedInput.Value
        $changed = & $identityTool -QtRoot $changedRoot `
            -ContractPath $contractPath | ConvertFrom-Json
        Assert-NotEqual $identityA.canonicalTreeSha256 `
            $changed.canonicalTreeSha256 `
            "A consumed Qt $($consumedInput.Kind) change did not change canonical identity."
    }

    $downgraded = Join-Path $temporaryRoot 'downgraded.json'
    Write-Contract $downgraded -SchemaVersion 0
    Assert-Rejected {
        & $identityTool -QtRoot $qtA -ContractPath $downgraded | Out-Null
    } '*schemaVersion*unsupported*'

    $extra = Join-Path $temporaryRoot 'extra-exclusion.json'
    Write-Contract $extra -ExcludedPaths @(
        [ordered]@{
            path = 'bin/qtenv2.bat'
            classification = 'qt-generated-environment-launcher'
            buildConsumption = 'proven-unused-release-x64'
            evidence = 'call-graph-and-file-io-v1'
        },
        [ordered]@{
            path = 'bin/qmake.exe'
            classification = 'unknown'
            buildConsumption = 'unknown'
            evidence = 'none'
        })
    Assert-Rejected {
        & $identityTool -QtRoot $qtA -ContractPath $extra | Out-Null
    } '*exactly one*bin/qtenv2.bat*'

    $duplicate = Join-Path $temporaryRoot 'duplicate-exclusion.json'
    Write-Contract $duplicate -ExcludedPaths @(
        [ordered]@{
            path = 'bin/qtenv2.bat'
            classification = 'qt-generated-environment-launcher'
            buildConsumption = 'proven-unused-release-x64'
            evidence = 'call-graph-and-file-io-v1'
        },
        [ordered]@{
            path = 'bin/qtenv2.bat'
            classification = 'qt-generated-environment-launcher'
            buildConsumption = 'proven-unused-release-x64'
            evidence = 'call-graph-and-file-io-v1'
        })
    Assert-Rejected {
        & $identityTool -QtRoot $qtA -ContractPath $duplicate | Out-Null
    } '*duplicate*'

    $traversal = Join-Path $temporaryRoot 'traversal-exclusion.json'
    Write-Contract $traversal -ExcludedPaths @(
        [ordered]@{
            path = 'bin/../bin/qtenv2.bat'
            classification = 'qt-generated-environment-launcher'
            buildConsumption = 'proven-unused-release-x64'
            evidence = 'call-graph-and-file-io-v1'
        })
    Assert-Rejected {
        & $identityTool -QtRoot $qtA -ContractPath $traversal | Out-Null
    } '*path*invalid*'

    $caseMismatch = Join-Path $temporaryRoot 'case-exclusion.json'
    Write-Contract $caseMismatch -ExcludedPaths @(
        [ordered]@{
            path = 'BIN/qtenv2.bat'
            classification = 'qt-generated-environment-launcher'
            buildConsumption = 'proven-unused-release-x64'
            evidence = 'call-graph-and-file-io-v1'
        })
    Assert-Rejected {
        & $identityTool -QtRoot $qtA -ContractPath $caseMismatch | Out-Null
    } '*exact case*bin/qtenv2.bat*'

    $unknown = Join-Path $temporaryRoot 'unknown-property.json'
    Write-Contract $unknown -IncludeUnknownProperty
    Assert-Rejected {
        & $identityTool -QtRoot $qtA -ContractPath $unknown | Out-Null
    } '*unknown contract propert*'

    Remove-Item -LiteralPath (Join-Path $qtA 'bin\qtenv2.bat') -Force
    Assert-Rejected {
        & $identityTool -QtRoot $qtA -ContractPath $contractPath | Out-Null
    } '*excluded Qt path is missing*'

    Write-Output 'Qt canonical identity contract tests passed.'
}
finally {
    if (Test-Path -LiteralPath $temporaryRoot) {
        Remove-Item -LiteralPath $temporaryRoot -Recurse -Force
    }
}
