[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $UpdatesXmlPath,

    [Parameter(Mandatory = $true)]
    [string] $ArchiveDirectory,

    [Parameter(Mandatory = $true)]
    [string] $ContractPath,

    [Parameter(Mandatory = $true)]
    [string] $OutputPath
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Get-RequiredProperty {
    param([Text.Json.JsonElement] $Object, [string] $Name)

    $value = [Text.Json.JsonElement]::new()
    if (-not $Object.TryGetProperty($Name, [ref] $value)) {
        throw "The Qt package object contract is missing '$Name'."
    }
    return $value
}

function Assert-ExactProperties {
    param(
        [Text.Json.JsonElement] $Object,
        [string[]] $Expected,
        [string] $Description)

    $seen = [Collections.Generic.HashSet[string]]::new(
        [StringComparer]::Ordinal)
    foreach ($property in $Object.EnumerateObject()) {
        if (-not $seen.Add($property.Name)) {
            throw "The Qt package object contract contains duplicate $Description property '$($property.Name)'."
        }
        if ($property.Name -cnotin $Expected) {
            throw "The Qt package object contract contains unknown $Description property '$($property.Name)'."
        }
    }
    foreach ($name in $Expected) {
        if (-not $seen.Contains($name)) {
            throw "The Qt package object contract is missing $Description property '$name'."
        }
    }
}

function Get-Sha256 {
    param([string] $Path)

    return (Get-FileHash -Algorithm SHA256 -LiteralPath $Path).Hash
}

$updatesXml = (Resolve-Path -LiteralPath $UpdatesXmlPath).Path
$archiveRoot = (Resolve-Path -LiteralPath $ArchiveDirectory).Path
$contract = (Resolve-Path -LiteralPath $ContractPath).Path
$contractBytes = [IO.File]::ReadAllBytes($contract)
$document = $null
$temporaryOutput = $null
try {
    $document = [Text.Json.JsonDocument]::Parse(
        [ReadOnlyMemory[byte]]::new($contractBytes))
    $root = $document.RootElement
    if ($root.ValueKind -ne [Text.Json.JsonValueKind]::Object) {
        throw 'The Qt package object contract root must be an object.'
    }
    Assert-ExactProperties $root @(
        'schemaVersion', 'algorithm', 'package', 'archives') 'root'
    if ((Get-RequiredProperty $root 'schemaVersion').GetInt32() -ne 1) {
        throw 'The Qt package object contract schemaVersion is unsupported.'
    }
    $algorithm = (Get-RequiredProperty $root 'algorithm').GetString()
    if ($algorithm -cne 'sha256-qt-package-object-set-v1') {
        throw 'The Qt package object contract algorithm is unsupported.'
    }

    $expectedPackage = Get-RequiredProperty $root 'package'
    Assert-ExactProperties $expectedPackage @(
        'name', 'version', 'releaseDate', 'sha1') 'package'
    $packageName = (Get-RequiredProperty $expectedPackage 'name').GetString()
    $packageVersion = (Get-RequiredProperty $expectedPackage 'version').GetString()
    $releaseDate = (Get-RequiredProperty $expectedPackage 'releaseDate').GetString()
    $packageSha1 = (Get-RequiredProperty $expectedPackage 'sha1').GetString()
    if ($packageSha1 -cnotmatch '^[0-9A-F]{40}$') {
        throw 'The Qt package object contract package SHA-1 is invalid.'
    }

    $expectedArchivesElement = Get-RequiredProperty $root 'archives'
    if ($expectedArchivesElement.ValueKind -ne [Text.Json.JsonValueKind]::Array) {
        throw 'The Qt package object contract archives must be an array.'
    }
    $expectedArchives = @($expectedArchivesElement.EnumerateArray())
    if ($expectedArchives.Count -eq 0) {
        throw 'The Qt package object contract must contain archives.'
    }
    $seenMetadataNames = [Collections.Generic.HashSet[string]]::new(
        [StringComparer]::OrdinalIgnoreCase)
    foreach ($archive in $expectedArchives) {
        Assert-ExactProperties $archive @(
            'metadataName', 'length', 'sha1', 'sha256') 'archive'
        $metadataName = (Get-RequiredProperty $archive 'metadataName').GetString()
        if ([string]::IsNullOrWhiteSpace($metadataName) -or
            $metadataName.Contains('/') -or
            $metadataName.Contains('\') -or
            $metadataName.Contains('..')) {
            throw 'The Qt package object contract archive name is invalid.'
        }
        if (-not $seenMetadataNames.Add($metadataName)) {
            throw "The Qt package object contract contains duplicate archive '$metadataName'."
        }
    }

    [xml] $metadata = Get-Content -Raw -LiteralPath $updatesXml
    $matchingPackages = @($metadata.Updates.PackageUpdate | Where-Object {
            [string]$_.Name -ceq $packageName
        })
    if ($matchingPackages.Count -ne 1) {
        throw 'Updates.xml must contain exactly one matching Qt package.'
    }
    $metadataPackage = $matchingPackages[0]
    if ([string]$metadataPackage.Version -cne $packageVersion -or
        [string]$metadataPackage.ReleaseDate -cne $releaseDate -or
        ([string]$metadataPackage.SHA1).ToUpperInvariant() -cne $packageSha1) {
        throw 'Updates.xml Qt package identity does not match the contract.'
    }
    $metadataArchiveNames = @(
        ([string]$metadataPackage.DownloadableArchives).Split(',') |
            ForEach-Object Trim)
    $contractArchiveNames = @($expectedArchives | ForEach-Object {
            (Get-RequiredProperty $_ 'metadataName').GetString()
        })
    if ($metadataArchiveNames.Count -ne $contractArchiveNames.Count) {
        throw 'Updates.xml Qt archive count does not match the contract.'
    }
    for ($index = 0; $index -lt $contractArchiveNames.Count; $index++) {
        if ($metadataArchiveNames[$index] -cne $contractArchiveNames[$index]) {
            throw 'Updates.xml Qt archive order or name does not match the contract.'
        }
    }

    $receiptArchives = [Collections.Generic.List[object]]::new()
    $expectedFileNames = [Collections.Generic.HashSet[string]]::new(
        [StringComparer]::OrdinalIgnoreCase)
    foreach ($archive in $expectedArchives) {
        $metadataName = (Get-RequiredProperty $archive 'metadataName').GetString()
        $objectName = "$packageVersion$metadataName"
        if (-not $expectedFileNames.Add($objectName)) {
            throw "The Qt package object contract resolves duplicate object '$objectName'."
        }
        $sidecarName = "$objectName.sha1"
        if (-not $expectedFileNames.Add($sidecarName)) {
            throw "The Qt package object contract resolves duplicate sidecar '$sidecarName'."
        }
        $objectPath = Join-Path $archiveRoot $objectName
        if (-not (Test-Path -LiteralPath $objectPath -PathType Leaf)) {
            throw "The Qt package archive object '$objectName' is missing."
        }
        $expectedLength = (Get-RequiredProperty $archive 'length').GetInt64()
        $actualLength = (Get-Item -LiteralPath $objectPath).Length
        $expectedSha1 = (Get-RequiredProperty $archive 'sha1').GetString()
        $expectedSha256 = (Get-RequiredProperty $archive 'sha256').GetString()
        $actualSha1 = (Get-FileHash -Algorithm SHA1 -LiteralPath $objectPath).Hash
        $actualSha256 = Get-Sha256 $objectPath
        if ($actualLength -ne $expectedLength -or
            $actualSha1 -cne $expectedSha1 -or
            $actualSha256 -cne $expectedSha256) {
            throw "The Qt package archive object '$objectName' length or hash does not match the contract."
        }
        $sidecarPath = Join-Path $archiveRoot $sidecarName
        if (-not (Test-Path -LiteralPath $sidecarPath -PathType Leaf)) {
            throw "The official Qt SHA-1 sidecar '$sidecarName' is missing."
        }
        $sidecarValue = (Get-Content -Raw -LiteralPath $sidecarPath).Trim()
        $sidecarSha1 = ($sidecarValue -split '\s+')[0].ToUpperInvariant()
        if ($sidecarSha1 -cne $expectedSha1) {
            throw "The official Qt SHA-1 sidecar '$sidecarName' does not match the contract."
        }
        $receiptArchives.Add([ordered]@{
                metadataName = $metadataName
                objectName = $objectName
                length = $actualLength
                sha1 = $actualSha1
                sha256 = $actualSha256
                officialSha1SidecarSha256 = Get-Sha256 $sidecarPath
            })
    }

    $actualObjects = @(Get-ChildItem -LiteralPath $archiveRoot -File |
            ForEach-Object Name)
    foreach ($actualObject in $actualObjects) {
        if (-not $expectedFileNames.Contains($actualObject)) {
            throw "The Qt package archive directory contains extraneous object '$actualObject'."
        }
    }
    if ($actualObjects.Count -ne $expectedFileNames.Count) {
        throw 'The Qt package archive object count does not match the contract.'
    }

    $identityHash = [Security.Cryptography.IncrementalHash]::CreateHash(
        [Security.Cryptography.HashAlgorithmName]::SHA256)
    try {
        foreach ($value in @($packageName, $packageVersion, $releaseDate, $packageSha1)) {
            $identityHash.AppendData([Text.Encoding]::UTF8.GetBytes($value))
            $identityHash.AppendData([byte[]] 10)
        }
        foreach ($archive in $receiptArchives) {
            $identityHash.AppendData([Text.Encoding]::UTF8.GetBytes($archive.metadataName))
            $identityHash.AppendData([byte[]] 0)
            $identityHash.AppendData([Text.Encoding]::ASCII.GetBytes($archive.sha256))
            $identityHash.AppendData([byte[]] 10)
        }
        $archiveSetSha256 = [Convert]::ToHexString(
            $identityHash.GetHashAndReset())
    }
    finally {
        $identityHash.Dispose()
    }

    $receipt = [ordered]@{
        schemaVersion = 1
        algorithm = $algorithm
        contractSha256 = [Convert]::ToHexString(
            [Security.Cryptography.SHA256]::HashData($contractBytes))
        archiveSetSha256 = $archiveSetSha256
        metadata = [ordered]@{
            updatesXmlSha256 = Get-Sha256 $updatesXml
        }
        package = [ordered]@{
            name = $packageName
            version = $packageVersion
            releaseDate = $releaseDate
            sha1 = $packageSha1
        }
        archives = @($receiptArchives)
    }
    $outputFullPath = [IO.Path]::GetFullPath($OutputPath)
    $outputParent = Split-Path -Parent $outputFullPath
    [IO.Directory]::CreateDirectory($outputParent) | Out-Null
    $temporaryOutput = Join-Path $outputParent `
        ".$([IO.Path]::GetFileName($outputFullPath)).$([Guid]::NewGuid().ToString('N')).tmp"
    $json = "$(ConvertTo-Json $receipt -Depth 8)`n"
    [IO.File]::WriteAllText(
        $temporaryOutput,
        $json,
        [Text.UTF8Encoding]::new($false))
    [IO.File]::Move($temporaryOutput, $outputFullPath, $true)
    $temporaryOutput = $null
}
finally {
    if ($null -ne $temporaryOutput -and
        (Test-Path -LiteralPath $temporaryOutput)) {
        Remove-Item -LiteralPath $temporaryOutput -Force
    }
    if ($null -ne $document) {
        $document.Dispose()
    }
    [Array]::Clear($contractBytes)
}
