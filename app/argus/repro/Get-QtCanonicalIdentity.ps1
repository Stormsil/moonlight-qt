[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $QtRoot,

    [Parameter(Mandatory = $true)]
    [string] $ContractPath
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Get-Sha256 {
    param([string] $Path)

    return (Get-FileHash -Algorithm SHA256 -LiteralPath $Path).Hash
}

function Get-RequiredProperty {
    param(
        [Text.Json.JsonElement] $Object,
        [string] $Name)

    $value = [Text.Json.JsonElement]::new()
    if (-not $Object.TryGetProperty($Name, [ref] $value)) {
        throw "The Qt identity contract is missing '$Name'."
    }
    return $value
}

function Assert-ExactProperties {
    param(
        [Text.Json.JsonElement] $Object,
        [string[]] $Expected,
        [string] $Description)

    $names = @($Object.EnumerateObject() | ForEach-Object Name)
    $seen = [Collections.Generic.HashSet[string]]::new(
        [StringComparer]::Ordinal)
    foreach ($name in $names) {
        if (-not $seen.Add($name)) {
            throw "The Qt identity contract contains duplicate $Description property '$name'."
        }
        if ($name -cnotin $Expected) {
            throw "The Qt identity contract contains unknown $Description property '$name'."
        }
    }
    foreach ($name in $Expected) {
        if (-not $seen.Contains($name)) {
            throw "The Qt identity contract is missing $Description property '$name'."
        }
    }
}

function Get-CanonicalFilePaths {
    param([string] $Root)

    $paths = [Collections.Generic.List[string]]::new()
    $pending = [Collections.Generic.Queue[string]]::new()
    $pending.Enqueue($Root)
    while ($pending.Count -gt 0) {
        $directory = $pending.Dequeue()
        foreach ($entry in [IO.Directory]::EnumerateFileSystemEntries(
                $directory)) {
            $attributes = [IO.File]::GetAttributes($entry)
            if (($attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
                throw 'The Qt input tree contains a reparse point.'
            }
            if (($attributes -band [IO.FileAttributes]::Directory) -ne 0) {
                $pending.Enqueue($entry)
                continue
            }
            $relative = [IO.Path]::GetRelativePath($Root, $entry).
                Replace('\', '/')
            if ([IO.Path]::IsPathFullyQualified($relative) -or
                $relative.Split('/') -ccontains '..') {
                throw "The Qt input path '$relative' escapes its root."
            }
            $paths.Add($relative)
        }
    }

    $caseInsensitive = [Collections.Generic.HashSet[string]]::new(
        [StringComparer]::OrdinalIgnoreCase)
    foreach ($path in $paths) {
        if (-not $caseInsensitive.Add($path)) {
            throw "The Qt input tree contains case-ambiguous path '$path'."
        }
    }
    $ordered = $paths.ToArray()
    [Array]::Sort($ordered, [StringComparer]::Ordinal)
    return $ordered
}

$qt = (Resolve-Path -LiteralPath $QtRoot).Path
$contract = (Resolve-Path -LiteralPath $ContractPath).Path
$contractBytes = [IO.File]::ReadAllBytes($contract)
$document = $null
try {
    $document = [Text.Json.JsonDocument]::Parse(
        [ReadOnlyMemory[byte]]::new($contractBytes))
    $root = $document.RootElement
    if ($root.ValueKind -ne [Text.Json.JsonValueKind]::Object) {
        throw 'The Qt identity contract root must be an object.'
    }
    Assert-ExactProperties $root @(
        'schemaVersion',
        'algorithm',
        'pathComparison',
        'excludedPaths') 'contract'

    $schemaVersion = Get-RequiredProperty $root 'schemaVersion'
    if ($schemaVersion.ValueKind -ne [Text.Json.JsonValueKind]::Number -or
        $schemaVersion.GetInt32() -ne 1) {
        throw 'The Qt identity contract schemaVersion is unsupported.'
    }
    $algorithm = (Get-RequiredProperty $root 'algorithm').GetString()
    if ($algorithm -cne 'sha256-relative-path-nul-content-sha256-lf-v1') {
        throw 'The Qt identity contract algorithm is unsupported.'
    }
    $pathComparison = (Get-RequiredProperty $root 'pathComparison').GetString()
    if ($pathComparison -cne 'ordinal') {
        throw 'The Qt identity contract path comparison is unsupported.'
    }

    $exclusions = Get-RequiredProperty $root 'excludedPaths'
    if ($exclusions.ValueKind -ne [Text.Json.JsonValueKind]::Array) {
        throw 'The Qt identity contract excludedPaths must be an array.'
    }
    $excludedEntries = @($exclusions.EnumerateArray())
    $seenExcludedPaths = [Collections.Generic.HashSet[string]]::new(
        [StringComparer]::OrdinalIgnoreCase)
    foreach ($entry in $excludedEntries) {
        if ($entry.ValueKind -ne [Text.Json.JsonValueKind]::Object) {
            throw 'The Qt identity contract exclusion must be an object.'
        }
        Assert-ExactProperties $entry @(
            'path',
            'classification',
            'buildConsumption',
            'evidence') 'exclusion'
        $path = (Get-RequiredProperty $entry 'path').GetString()
        if ($null -eq $path -or
            [IO.Path]::IsPathFullyQualified($path) -or
            $path.Contains('\', [StringComparison]::Ordinal) -or
            $path.Split('/') -ccontains '..') {
            throw 'The Qt identity contract exclusion path is invalid.'
        }
        if (-not $seenExcludedPaths.Add($path)) {
            throw "The Qt identity contract contains duplicate excluded path '$path'."
        }
    }
    if ($excludedEntries.Count -ne 1) {
        throw 'The Qt identity contract must exclude exactly one bin/qtenv2.bat path.'
    }

    $excluded = $excludedEntries[0]
    $excludedPath = (Get-RequiredProperty $excluded 'path').GetString()
    if ($excludedPath -cne 'bin/qtenv2.bat') {
        if ([string]::Equals(
                $excludedPath,
                'bin/qtenv2.bat',
                [StringComparison]::OrdinalIgnoreCase)) {
            throw 'The Qt identity exclusion must use exact case bin/qtenv2.bat.'
        }
        throw 'The Qt identity contract must exclude exactly one bin/qtenv2.bat path.'
    }
    if ((Get-RequiredProperty $excluded 'classification').GetString() -cne
            'qt-generated-environment-launcher' -or
        (Get-RequiredProperty $excluded 'buildConsumption').GetString() -cne
            'proven-unused-release-x64' -or
        (Get-RequiredProperty $excluded 'evidence').GetString() -cne
            'call-graph-and-file-io-v1') {
        throw 'The Qt identity contract exclusion evidence is unsupported.'
    }

    $allPaths = Get-CanonicalFilePaths $qt
    if (-not ($allPaths -ccontains $excludedPath)) {
        throw 'The excluded Qt path is missing from the input tree.'
    }

    $includedPaths = @($allPaths | Where-Object { $_ -cne $excludedPath })
    $hash = [Security.Cryptography.IncrementalHash]::CreateHash(
        [Security.Cryptography.HashAlgorithmName]::SHA256)
    try {
        foreach ($relative in $includedPaths) {
            $fullPath = Join-Path $qt $relative.Replace('/', '\')
            $fileHash = (Get-Sha256 $fullPath).ToLowerInvariant()
            $hash.AppendData([Text.Encoding]::UTF8.GetBytes($relative))
            $hash.AppendData([byte[]] 0)
            $hash.AppendData([Text.Encoding]::ASCII.GetBytes($fileHash))
            $hash.AppendData([byte[]] 10)
        }
        $canonicalTreeSha256 = [Convert]::ToHexString(
            $hash.GetHashAndReset())
    }
    finally {
        $hash.Dispose()
    }

    [ordered]@{
        schemaVersion = 1
        algorithm = $algorithm
        contractSha256 = [Convert]::ToHexString(
            [Security.Cryptography.SHA256]::HashData($contractBytes))
        canonicalTreeSha256 = $canonicalTreeSha256
        excludedPaths = @($excludedPath)
        includedFileCount = $includedPaths.Count
    } | ConvertTo-Json -Depth 4
}
finally {
    if ($null -ne $document) {
        $document.Dispose()
    }
    [Array]::Clear($contractBytes)
}
