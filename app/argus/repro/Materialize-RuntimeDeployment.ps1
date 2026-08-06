[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [string] $WorkerBuildRoot,
    [Parameter(Mandatory = $true)] [string] $WorkerReproducibilityProofPath,
    [Parameter(Mandatory = $true)] [string] $AntiHookingPath,
    [Parameter(Mandatory = $true)] [string] $QtRoot,
    [Parameter(Mandatory = $true)] [string] $SourceRoot,
    [Parameter(Mandatory = $true)] [string] $ToolchainRoot,
    [Parameter(Mandatory = $true)] [string] $RuntimeRoot,
    [Parameter(Mandatory = $true)] [string] $ReceiptPath,
    [string] $SourceAuthorityRoot = (Resolve-Path (
        Join-Path $PSScriptRoot '..\..\..')).Path)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Get-Sha256([string] $Path) {
    (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash
}

function Assert-Hash([string] $Path, [string] $Expected, [string] $Name) {
    $actual = Get-Sha256 $Path
    if ($actual -cne $Expected) {
        throw "$Name drifted: expected $Expected, got $actual."
    }
}

function Assert-ExactProperties(
    [object] $Value,
    [string[]] $Expected,
    [string] $Name) {
    $actual = @($Value.PSObject.Properties.Name)
    [Array]::Sort($actual, [StringComparer]::Ordinal)
    $orderedExpected = @($Expected)
    [Array]::Sort($orderedExpected, [StringComparer]::Ordinal)
    if (($actual -join "`n") -cne ($orderedExpected -join "`n")) {
        throw "$Name properties are unsupported."
    }
}

function Get-TreeSha256([string] $Root) {
    $rootPath = (Resolve-Path -LiteralPath $Root).Path
    $paths = @(Get-ChildItem -LiteralPath $rootPath -Recurse -File -Force |
        ForEach-Object {
            [IO.Path]::GetRelativePath($rootPath, $_.FullName).Replace('\', '/')
        })
    [Array]::Sort($paths, [StringComparer]::Ordinal)
    $hash = [Security.Cryptography.IncrementalHash]::CreateHash(
        [Security.Cryptography.HashAlgorithmName]::SHA256)
    try {
        foreach ($relative in $paths) {
            $hash.AppendData([Text.Encoding]::UTF8.GetBytes($relative))
            $hash.AppendData([byte[]] 0)
            $hash.AppendData([Text.Encoding]::ASCII.GetBytes(
                (Get-Sha256 (Join-Path $rootPath $relative.Replace('/', '\'))).
                    ToLowerInvariant()))
            $hash.AppendData([byte[]] 10)
        }
        [Convert]::ToHexString($hash.GetHashAndReset())
    }
    finally { $hash.Dispose() }
}

function Get-OrdinalSortedObjects([object[]] $Values, [string] $Property) {
    $ordered = [object[]]@($Values)
    $comparer = [Collections.Generic.Comparer[object]]::Create(
        [Comparison[object]] {
            param($left, $right)
            [StringComparer]::Ordinal.Compare(
                [string]$left.$Property,
                [string]$right.$Property)
        })
    [Array]::Sort($ordered, $comparer)
    $ordered
}

function Get-OrdinalSortedStrings([string[]] $Values) {
    $unique = [Collections.Generic.HashSet[string]]::new(
        [StringComparer]::OrdinalIgnoreCase)
    foreach ($value in $Values) { $unique.Add($value) | Out-Null }
    $ordered = [string[]]@($unique)
    [Array]::Sort($ordered, [StringComparer]::OrdinalIgnoreCase)
    $ordered
}

function Get-SelectedTreeSha256([object[]] $Files) {
    $ordered = Get-OrdinalSortedObjects $Files 'RelativePath'
    $hash = [Security.Cryptography.IncrementalHash]::CreateHash(
        [Security.Cryptography.HashAlgorithmName]::SHA256)
    try {
        foreach ($file in $ordered) {
            $hash.AppendData([Text.Encoding]::UTF8.GetBytes('F'))
            $hash.AppendData([byte[]] 0)
            $hash.AppendData([Text.Encoding]::UTF8.GetBytes($file.RelativePath))
            $hash.AppendData([byte[]] 0)
            $hash.AppendData([Text.Encoding]::ASCII.GetBytes(
                ([string]$file.Length)))
            $hash.AppendData([byte[]] 0)
            $hash.AppendData([Text.Encoding]::ASCII.GetBytes($file.Sha256))
            $hash.AppendData([byte[]] 10)
        }
        [Convert]::ToHexString($hash.GetHashAndReset())
    }
    finally { $hash.Dispose() }
}

function Get-RuntimeTreeSha256([string[]] $Directories, [object[]] $Files) {
    $hash = [Security.Cryptography.IncrementalHash]::CreateHash(
        [Security.Cryptography.HashAlgorithmName]::SHA256)
    try {
        foreach ($directory in $Directories) {
            $hash.AppendData([Text.Encoding]::UTF8.GetBytes('D'))
            $hash.AppendData([byte[]] 0)
            $hash.AppendData([Text.Encoding]::UTF8.GetBytes($directory))
            $hash.AppendData([byte[]] 10)
        }
        foreach ($file in $Files) {
            $hash.AppendData([Text.Encoding]::UTF8.GetBytes('F'))
            $hash.AppendData([byte[]] 0)
            $hash.AppendData([Text.Encoding]::UTF8.GetBytes($file.relativePath))
            $hash.AppendData([byte[]] 0)
            $hash.AppendData([Text.Encoding]::ASCII.GetBytes(
                ([string]$file.length)))
            $hash.AppendData([byte[]] 0)
            $hash.AppendData([Text.Encoding]::ASCII.GetBytes($file.sha256))
            $hash.AppendData([byte[]] 10)
        }
        [Convert]::ToHexString($hash.GetHashAndReset())
    }
    finally { $hash.Dispose() }
}

function Resolve-QtSourcePath(
    [string] $RelativePath,
    [string] $Qt,
    [string] $RuntimeFile) {
    $candidates = [Collections.Generic.List[string]]::new()
    if (-not $RelativePath.Contains('/')) {
        $candidates.Add((Join-Path $Qt "bin\$RelativePath"))
    }
    $candidates.Add((Join-Path $Qt `
        "plugins\$($RelativePath.Replace('/', '\'))"))
    foreach ($prefix in @('qml', 'translations')) {
        if ($RelativePath.StartsWith(
                "$prefix/",
                [StringComparison]::Ordinal)) {
            $withoutPrefix = $RelativePath.Substring($prefix.Length + 1)
            $candidates.Add((Join-Path $Qt `
                "$prefix\$($withoutPrefix.Replace('/', '\'))"))
        }
    }
    $matches = @($candidates | Where-Object {
        Test-Path -LiteralPath $_ -PathType Leaf
    } | Where-Object { (Get-Sha256 $_) -ceq (Get-Sha256 $RuntimeFile) })
    if ($matches.Count -gt 1) {
        throw "Runtime file '$RelativePath' has no unique Qt provenance."
    }
    if ($matches.Count -eq 0) {
        return $null
    }
    [IO.Path]::GetRelativePath($Qt, $matches[0]).Replace('\', '/')
}

function Write-AtomicJson([object] $Value, [string] $Path) {
    $full = [IO.Path]::GetFullPath($Path)
    [IO.Directory]::CreateDirectory((Split-Path -Parent $full)) | Out-Null
    $temporary = "$full.$([Guid]::NewGuid().ToString('N')).tmp"
    try {
        [IO.File]::WriteAllText(
            $temporary,
            "$(ConvertTo-Json $Value -Depth 24)`n",
            [Text.UTF8Encoding]::new($false))
        [IO.File]::Move($temporary, $full, $false)
    }
    finally {
        if (Test-Path -LiteralPath $temporary) {
            Remove-Item -LiteralPath $temporary -Force
        }
    }
}

$build = (Resolve-Path -LiteralPath $WorkerBuildRoot).Path
$proofPath = (Resolve-Path -LiteralPath $WorkerReproducibilityProofPath).Path
$antiHookSource = (Resolve-Path -LiteralPath $AntiHookingPath).Path
$qt = (Resolve-Path -LiteralPath $QtRoot).Path
$source = (Resolve-Path -LiteralPath $SourceRoot).Path
$toolchain = (Resolve-Path -LiteralPath $ToolchainRoot).Path
$authority = (Resolve-Path -LiteralPath $SourceAuthorityRoot).Path
$runtime = [IO.Path]::GetFullPath($RuntimeRoot)
$receiptOutput = [IO.Path]::GetFullPath($ReceiptPath)
if (Test-Path -LiteralPath $runtime) {
    throw 'RuntimeRoot must not exist; materialization always starts clean.'
}
if (Test-Path -LiteralPath $receiptOutput) {
    throw 'ReceiptPath must not exist.'
}
if ($runtime.StartsWith($build, [StringComparison]::OrdinalIgnoreCase) -or
    $runtime.StartsWith($source, [StringComparison]::OrdinalIgnoreCase) -or
    $runtime.StartsWith($qt, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'RuntimeRoot must be independent of all input roots.'
}

$contractPath = Join-Path $authority 'app\argus\repro\runtime-deployment-v1.json'
$contract = Get-Content -Raw -LiteralPath $contractPath | ConvertFrom-Json
Assert-ExactProperties $contract @(
    'schemaVersion', 'manifestSchemaVersion', 'inventoryAlgorithm',
    'requiredAncestor', 'worker', 'antiHooking', 'qt', 'dependencies',
    'msvcRuntime', 'peImportTool',
    'windowsSystemLibraries', 'ambientPathAllowed', 'distributionAllowed',
    'legalApproval') 'Runtime deployment contract'
Assert-ExactProperties $contract.worker @('fileName', 'sha256', 'length') `
    'Runtime worker contract'
Assert-ExactProperties $contract.antiHooking @('fileName', 'sha256', 'length') `
    'Runtime AntiHooking contract'
Assert-ExactProperties $contract.qt @(
    'treeSha256', 'windeployqtRelativePath', 'windeployqtSha256',
    'windeployqtVersion', 'peImportClosureSourceRelativePath', 'postCopyRules',
    'arguments', 'requiredRuntimePaths') `
    'Runtime Qt contract'
foreach ($rule in $contract.qt.postCopyRules) {
    Assert-ExactProperties $rule @(
        'sourceRelativePath', 'targetRelativePath', 'sha256', 'length') `
        'Runtime Qt post-copy rule'
}
Assert-ExactProperties $contract.dependencies @('treeSha256', 'copyGlob') `
    'Runtime dependency contract'
Assert-ExactProperties $contract.msvcRuntime @('treeSha256', 'copyRules') `
    'Runtime MSVC contract'
foreach ($rule in $contract.msvcRuntime.copyRules) {
    Assert-ExactProperties $rule @(
        'sourceRelativePath', 'targetRelativePath', 'sha256', 'length') `
        'Runtime MSVC copy rule'
}
Assert-ExactProperties $contract.peImportTool @('relativePath', 'sha256') `
    'Runtime PE import contract'
if ($contract.schemaVersion -ne 1 -or
    $contract.manifestSchemaVersion -ne 6 -or
    $contract.ambientPathAllowed -ne $false -or
    $contract.distributionAllowed -ne $false -or
    $contract.legalApproval -ne $false) {
    throw 'Runtime deployment contract is unsupported.'
}
$status = @(& git -C $authority status --porcelain=v1 --untracked-files=all)
if ($LASTEXITCODE -ne 0 -or $status.Count -ne 0) {
    throw 'SourceAuthorityRoot must be a clean Git worktree.'
}
$producerCommit = (& git -C $authority rev-parse HEAD).Trim()
$producerTree = (& git -C $authority rev-parse 'HEAD^{tree}').Trim()
$contractSha256 = Get-Sha256 $contractPath
& git -C $authority merge-base --is-ancestor $contract.requiredAncestor HEAD
if ($LASTEXITCODE -ne 0) {
    throw 'Runtime deployment source does not descend from the required producer.'
}

$buildReceiptPath = Join-Path $build 'artifact\reproducible-worker-receipt.json'
$buildReceipt = Get-Content -Raw -LiteralPath $buildReceiptPath | ConvertFrom-Json
$proof = Get-Content -Raw -LiteralPath $proofPath | ConvertFrom-Json
$workerSource = Join-Path $build 'artifact\Moonlight.exe'
$gameControllerSource = Join-Path $source `
    'app\SDL_GameControllerDB\gamecontrollerdb.txt'
if ($buildReceipt.schemaVersion -ne 2 -or $proof.schemaVersion -ne 2 -or
    $buildReceipt.source.forkCommit -cne $proof.sourceCommit -or
    $buildReceipt.source.sourceTree -cne $proof.sourceTree -or
    $buildReceipt.contractSha256 -cne $proof.contractSha256 -or
    $proof.worker.byteForByteIdentical -ne $true) {
    throw 'Worker build receipt and reproducibility proof do not agree.'
}
Assert-Hash $workerSource $contract.worker.sha256 'Moonlight.exe'
if ((Get-Item -LiteralPath $workerSource).Length -ne $contract.worker.length) {
    throw 'Moonlight.exe length drifted.'
}
Assert-Hash $antiHookSource $contract.antiHooking.sha256 'AntiHooking.dll'
if ((Get-Item -LiteralPath $antiHookSource).Length -ne
    $contract.antiHooking.length) {
    throw 'AntiHooking.dll length drifted.'
}
if ($proof.worker.sha256 -cne $contract.worker.sha256 -or
    $proof.worker.length -ne $contract.worker.length) {
    throw 'Worker reproducibility proof does not bind the required worker.'
}

$qtIdentityTool = Join-Path $authority `
    'app\argus\repro\Get-QtCanonicalIdentity.ps1'
$qtIdentityContract = Join-Path $authority `
    'app\argus\repro\qt-canonical-identity-v1.json'
$qtIdentity = & $qtIdentityTool -QtRoot $qt `
    -ContractPath $qtIdentityContract | ConvertFrom-Json
if ($qtIdentity.canonicalTreeSha256 -cne $contract.qt.treeSha256) {
    throw 'Qt runtime input tree drifted.'
}
$dependenciesRoot = Join-Path $source 'libs\windows'
if ((Get-TreeSha256 $dependenciesRoot) -cne
    $contract.dependencies.treeSha256) {
    throw 'Runtime dependency tree drifted.'
}
$msvcRoot = Join-Path $toolchain 'msvc'
if ((Get-TreeSha256 $msvcRoot) -cne $contract.msvcRuntime.treeSha256) {
    throw 'Runtime MSVC input tree drifted.'
}
$windeployqt = Join-Path $qt `
    $contract.qt.windeployqtRelativePath.Replace('/', '\')
$dumpbin = Join-Path $toolchain `
    $contract.peImportTool.relativePath.Replace('/', '\')
Assert-Hash $windeployqt $contract.qt.windeployqtSha256 'windeployqt.exe'
Assert-Hash $dumpbin $contract.peImportTool.sha256 'dumpbin.exe'
if ((Get-Item -LiteralPath $windeployqt).VersionInfo.FileVersion -cne
    $contract.qt.windeployqtVersion) {
    throw 'windeployqt.exe version drifted.'
}

[IO.Directory]::CreateDirectory($runtime) | Out-Null
$workerTarget = Join-Path $runtime 'Moonlight.exe'
Copy-Item -LiteralPath $workerSource -Destination $workerTarget
Copy-Item -LiteralPath $antiHookSource `
    -Destination (Join-Path $runtime 'AntiHooking.dll')
Copy-Item -LiteralPath $gameControllerSource `
    -Destination (Join-Path $runtime 'gamecontrollerdb.txt')

$arguments = @(
    '--dir', $runtime,
    '--release',
    '--qmldir', (Join-Path $source 'app\gui'),
    '--no-opengl-sw',
    '--no-compiler-runtime',
    '--no-sql',
    '--no-system-d3d-compiler',
    '--no-system-dxc-compiler',
    '--skip-plugin-types', 'qmltooling,generic',
    '--no-ffmpeg',
    '--no-quickcontrols2fusion',
    '--no-quickcontrols2imagine',
    '--no-quickcontrols2universal',
    '--no-quickcontrols2fusionstyleimpl',
    '--no-quickcontrols2imaginestyleimpl',
    '--no-quickcontrols2universalstyleimpl',
    '--no-quickcontrols2windowsstyleimpl',
    '--no-quickcontrols2fluentwinui3styleimpl',
    $workerTarget)
$oldPath = $env:PATH
try {
    $env:PATH = @(
        (Join-Path $qt 'bin'),
        (Join-Path $toolchain 'msvc\bin\Hostx64\x64'),
        (Join-Path $toolchain 'windows-sdk\bin\x64'),
        (Join-Path $env:SystemRoot 'System32'),
        $env:SystemRoot) -join ';'
    $deployOutput = & $windeployqt @arguments 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "windeployqt failed with exit $LASTEXITCODE`: $($deployOutput -join [Environment]::NewLine)"
    }
}
finally { $env:PATH = $oldPath }

foreach ($rule in $contract.qt.postCopyRules) {
    $copySource = Join-Path $qt $rule.sourceRelativePath.Replace('/', '\')
    $copyTarget = Join-Path $runtime $rule.targetRelativePath.Replace('/', '\')
    Assert-Hash $copySource $rule.sha256 `
        "Qt post-copy source $($rule.sourceRelativePath)"
    if ((Get-Item -LiteralPath $copySource).Length -ne $rule.length) {
        throw "Qt post-copy source '$($rule.sourceRelativePath)' length drifted."
    }
    [IO.Directory]::CreateDirectory((Split-Path -Parent $copyTarget)) |
        Out-Null
    Copy-Item -LiteralPath $copySource -Destination $copyTarget -Force
}

$msvcRuntimeByName = @{}
foreach ($rule in $contract.msvcRuntime.copyRules) {
    $copySource = Join-Path $msvcRoot `
        $rule.sourceRelativePath.Replace('/', '\')
    $copyTarget = Join-Path $runtime `
        $rule.targetRelativePath.Replace('/', '\')
    Assert-Hash $copySource $rule.sha256 `
        "MSVC runtime source $($rule.sourceRelativePath)"
    if ((Get-Item -LiteralPath $copySource).Length -ne $rule.length) {
        throw "MSVC runtime source '$($rule.sourceRelativePath)' length drifted."
    }
    Copy-Item -LiteralPath $copySource -Destination $copyTarget
    $msvcRuntimeByName[$rule.targetRelativePath] = $rule
}

$dependencyDlls = @(Get-ChildItem -LiteralPath (
    Join-Path $dependenciesRoot 'lib\x64') -Filter *.dll -File)
foreach ($dependency in $dependencyDlls) {
    Copy-Item -LiteralPath $dependency.FullName -Destination $runtime -Force
}
$system = [Collections.Generic.HashSet[string]]::new(
    [StringComparer]::OrdinalIgnoreCase)
foreach ($name in $contract.windowsSystemLibraries) { $system.Add($name) | Out-Null }
$qtClosurePaths = [Collections.Generic.HashSet[string]]::new(
    [StringComparer]::Ordinal)
do {
    $addedQtImport = $false
    $runtimeNames = [Collections.Generic.HashSet[string]]::new(
        [StringComparer]::OrdinalIgnoreCase)
    Get-ChildItem -LiteralPath $runtime -Recurse -File |
        ForEach-Object { $runtimeNames.Add($_.Name) | Out-Null }
    $peFiles = @(Get-ChildItem -LiteralPath $runtime -Recurse -File |
        Where-Object { $_.Extension -in @('.exe', '.dll') })
    foreach ($peFile in $peFiles) {
        $importOutput = & $dumpbin /dependents $peFile.FullName 2>&1
        if ($LASTEXITCODE -ne 0) {
            throw "dumpbin failed for '$($peFile.FullName)'."
        }
        foreach ($line in $importOutput) {
            if ($line.ToString() -notmatch `
                '^\s+([A-Za-z0-9_.+\-]+\.(?:dll|drv))\s*$') {
                continue
            }
            $importName = $Matches[1]
            if ($runtimeNames.Contains($importName) -or
                $system.Contains($importName) -or
                $importName.StartsWith(
                    'api-ms-win-',
                    [StringComparison]::OrdinalIgnoreCase) -or
                $importName.StartsWith(
                    'ext-ms-win-',
                    [StringComparison]::OrdinalIgnoreCase)) {
                continue
            }
            $qtImport = Join-Path $qt `
                "$($contract.qt.peImportClosureSourceRelativePath)\$importName"
            if (-not (Test-Path -LiteralPath $qtImport -PathType Leaf)) {
                throw "PE import '$importName' has no pinned runtime source."
            }
            Copy-Item -LiteralPath $qtImport `
                -Destination (Join-Path $runtime $importName)
            $qtClosurePaths.Add($importName) | Out-Null
            $runtimeNames.Add($importName) | Out-Null
            $addedQtImport = $true
        }
    }
} while ($addedQtImport)
foreach ($required in $contract.qt.requiredRuntimePaths) {
    if (-not (Test-Path -LiteralPath (
            Join-Path $runtime $required.Replace('/', '\')) -PathType Leaf)) {
        throw "Required runtime path '$required' is missing."
    }
}

$directories = @(Get-ChildItem -LiteralPath $runtime -Recurse -Directory |
    ForEach-Object {
        if (($_.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Runtime directory '$($_.FullName)' is a reparse point."
        }
        [IO.Path]::GetRelativePath($runtime, $_.FullName).Replace('\', '/')
    })
[Array]::Sort($directories, [StringComparer]::Ordinal)
$dependencyByName = @{}
foreach ($dependency in $dependencyDlls) {
    $dependencyByName[$dependency.Name] = $dependency.FullName
}
$postCopyRules = [Collections.Generic.List[object]]::new()
$files = [Collections.Generic.List[object]]::new()
foreach ($file in Get-ChildItem -LiteralPath $runtime -Recurse -File) {
    if (($file.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "Runtime file '$($file.FullName)' is a reparse point."
    }
    $relative = [IO.Path]::GetRelativePath(
        $runtime, $file.FullName).Replace('\', '/')
    $component = 'qt'
    $sourceRelative = $null
    if ($relative -ceq 'Moonlight.exe') {
        $component = 'worker-build-output'
        $sourceRelative = 'artifact/Moonlight.exe'
    }
    elseif ($relative -ceq 'AntiHooking.dll') {
        $component = 'runtime-anti-hooking'
        $sourceRelative = 'artifact/AntiHooking.dll'
    }
    elseif ($relative -ceq 'gamecontrollerdb.txt') {
        $component = 'moonlight-source'
        $sourceRelative = 'app/SDL_GameControllerDB/gamecontrollerdb.txt'
    }
    elseif (-not $relative.Contains('/') -and
        $dependencyByName.ContainsKey($file.Name)) {
        $component = 'moonlight-qt-deps'
        $sourceRelative = "lib/x64/$($file.Name)"
        Assert-Hash $file.FullName (Get-Sha256 $dependencyByName[$file.Name]) `
            "runtime dependency $relative"
    }
    elseif (-not $relative.Contains('/') -and
        $msvcRuntimeByName.ContainsKey($relative)) {
        $component = 'msvc-runtime'
        $sourceRelative =
            $msvcRuntimeByName[$relative].sourceRelativePath
        Assert-Hash $file.FullName $msvcRuntimeByName[$relative].sha256 `
            "runtime MSVC dependency $relative"
    }
    else {
        $sourceRelative = Resolve-QtSourcePath $relative $qt $file.FullName
        if ($null -eq $sourceRelative) {
            if (-not $relative.StartsWith(
                    'translations/',
                    [StringComparison]::Ordinal)) {
                throw "Runtime file '$relative' has no Qt provenance."
            }
            $component = 'qt-windeploy-generated'
            $sourceRelative = "generated/$relative"
        }
    }
    $sha = Get-Sha256 $file.FullName
    $files.Add([ordered]@{
        relativePath = $relative
        length = $file.Length
        sha256 = $sha
        provenanceComponent = $component
    })
    $explicitQtCopy = @($contract.qt.postCopyRules | Where-Object {
        $_.targetRelativePath -ceq $relative
    }).Count -eq 1
    if ($component -in @(
            'worker-build-output',
            'runtime-anti-hooking',
            'moonlight-source',
            'moonlight-qt-deps',
            'msvc-runtime') -or
        $qtClosurePaths.Contains($relative) -or $explicitQtCopy) {
        $postCopyRules.Add([ordered]@{
            sourceComponent = $component
            sourceRelativePath = $sourceRelative
            targetRelativePath = $relative
            sha256 = $sha
        })
    }
}
$fileArray = Get-OrdinalSortedObjects $files.ToArray() 'relativePath'

$runtimeByName = @{}
foreach ($file in $fileArray) {
    $name = [IO.Path]::GetFileName($file.relativePath).ToLowerInvariant()
    if (-not $runtimeByName.ContainsKey($name)) {
        $runtimeByName[$name] = [Collections.Generic.List[string]]::new()
    }
    $runtimeByName[$name].Add($file.relativePath)
}
$peBinaries = [Collections.Generic.List[object]]::new()
foreach ($file in $fileArray | Where-Object {
        $_.relativePath.EndsWith('.exe', [StringComparison]::OrdinalIgnoreCase) -or
        $_.relativePath.EndsWith('.dll', [StringComparison]::OrdinalIgnoreCase) }) {
    $binaryPath = Join-Path $runtime $file.relativePath.Replace('/', '\')
    $output = & $dumpbin /dependents $binaryPath 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "dumpbin failed for '$($file.relativePath)'."
    }
    $imports = [Collections.Generic.List[object]]::new()
    $names = Get-OrdinalSortedStrings @($output | ForEach-Object {
        if ($_.ToString() -match '^\s+([A-Za-z0-9_.+\-]+\.(?:dll|drv))\s*$') {
            $Matches[1]
        }
    })
    foreach ($name in $names) {
        $key = $name.ToLowerInvariant()
        $resolved = $null
        if ($runtimeByName.ContainsKey($key)) {
            $candidates = @($runtimeByName[$key])
            $binaryDirectory = [IO.Path]::GetDirectoryName(
                $file.relativePath.Replace('/', '\'))
            $sameDirectory = if ([string]::IsNullOrEmpty($binaryDirectory)) {
                $name
            } else {
                "$($binaryDirectory.Replace('\', '/'))/$name"
            }
            $resolved = @($candidates | Where-Object {
                $_ -ceq $sameDirectory }) | Select-Object -First 1
            if ($null -eq $resolved) {
                $resolved = @($candidates | Where-Object {
                    $_ -ceq $name }) | Select-Object -First 1
            }
            if ($null -eq $resolved -and $candidates.Count -eq 1) {
                $resolved = $candidates[0]
            }
            if ($null -eq $resolved) {
                throw "PE import '$name' from '$($file.relativePath)' is ambiguous."
            }
            $imports.Add([ordered]@{
                libraryName = $name
                resolution = 'runtime'
                resolvedRelativePath = $resolved
            })
        }
        elseif ($system.Contains($name) -or
            $name.StartsWith('api-ms-win-', [StringComparison]::OrdinalIgnoreCase) -or
            $name.StartsWith('ext-ms-win-', [StringComparison]::OrdinalIgnoreCase)) {
            $imports.Add([ordered]@{
                libraryName = $name
                resolution = 'windows-system'
                resolvedRelativePath = $null
            })
        }
        else {
            throw "PE import '$name' from '$($file.relativePath)' is unresolved."
        }
    }
    $peBinaries.Add([ordered]@{
        relativePath = $file.relativePath
        imports = @($imports)
    })
}

$workerInputs = @($fileArray | Where-Object {
    $_.provenanceComponent -ceq 'worker-build-output' } | ForEach-Object {
        [pscustomobject]@{
            RelativePath = $_.relativePath
            Length = $_.length
            Sha256 = $_.sha256
        }
    })
$sourceInputs = @($fileArray | Where-Object {
    $_.provenanceComponent -ceq 'moonlight-source' } | ForEach-Object {
        [pscustomobject]@{
            RelativePath = $_.relativePath
            Length = $_.length
            Sha256 = $_.sha256
        }
    })
$antiHookInputs = @($fileArray | Where-Object {
    $_.provenanceComponent -ceq 'runtime-anti-hooking' } | ForEach-Object {
        [pscustomobject]@{
            RelativePath = $_.relativePath
            Length = $_.length
            Sha256 = $_.sha256
        }
    })
$finalStatus = @(& git -C $authority status --porcelain=v1 --untracked-files=all)
if ($LASTEXITCODE -ne 0 -or $finalStatus.Count -ne 0 -or
    (& git -C $authority rev-parse HEAD).Trim() -cne $producerCommit -or
    (& git -C $authority rev-parse 'HEAD^{tree}').Trim() -cne $producerTree -or
    (Get-Sha256 $contractPath) -cne $contractSha256) {
    throw 'Runtime deployment source authority changed during materialization.'
}
$receipt = [ordered]@{
    schemaVersion = 1
    manifestSchemaVersion = 6
    workerSha256 = $contract.worker.sha256
    nativeSourceCommit = $proof.sourceCommit
    nativeSourceTree = $proof.sourceTree
    runtimeContractSourceCommit = $producerCommit
    runtimeContractSourceTree = $producerTree
    buildContractSha256 = $proof.contractSha256
    workerReproducibilityProofSha256 = Get-Sha256 $proofPath
    qtTreeSha256 = $contract.qt.treeSha256
    dependencyTreeSha256 = $contract.dependencies.treeSha256
    inventoryAlgorithm = $contract.inventoryAlgorithm
    treeSha256 = Get-RuntimeTreeSha256 $directories $fileArray
    fileCount = $fileArray.Count
    directoryCount = $directories.Count
    materialization = [ordered]@{
        schemaVersion = 1
        windeployQtRelativePath = $contract.qt.windeployqtRelativePath
        windeployQtSha256 = $contract.qt.windeployqtSha256
        windeployQtVersion = $contract.qt.windeployqtVersion
        peImportToolRelativePath = $contract.peImportTool.relativePath
        peImportToolSha256 = $contract.peImportTool.sha256
        windeployQtFlags = @($contract.qt.arguments)
        sourceRoots = @(
            [ordered]@{ component = 'worker-build-output'; treeSha256 = Get-SelectedTreeSha256 $workerInputs },
            [ordered]@{ component = 'runtime-anti-hooking'; treeSha256 = Get-SelectedTreeSha256 $antiHookInputs },
            [ordered]@{ component = 'qt'; treeSha256 = $contract.qt.treeSha256 },
            [ordered]@{ component = 'qt-windeploy-generated'; treeSha256 = $contract.qt.treeSha256 },
            [ordered]@{ component = 'moonlight-qt-deps'; treeSha256 = $contract.dependencies.treeSha256 },
            [ordered]@{ component = 'msvc-runtime'; treeSha256 = $contract.msvcRuntime.treeSha256 },
            [ordered]@{ component = 'moonlight-source'; treeSha256 = Get-SelectedTreeSha256 $sourceInputs })
        postCopyRules = @(Get-OrdinalSortedObjects `
            $postCopyRules.ToArray() 'targetRelativePath')
        ambientPathAllowed = $false
    }
    directories = $directories
    files = $fileArray
    peBinaries = @($peBinaries)
    distributionAllowed = $false
    legalApproval = $false
}
Write-AtomicJson $receipt $receiptOutput
$receipt
