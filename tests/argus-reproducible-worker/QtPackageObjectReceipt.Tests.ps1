[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Write-Utf8File {
    param([string] $Path, [string] $Value)

    $parent = Split-Path -Parent $Path
    [IO.Directory]::CreateDirectory($parent) | Out-Null
    [IO.File]::WriteAllText($Path, $Value, [Text.UTF8Encoding]::new($false))
}

$sourceRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$receiptTool = Join-Path $sourceRoot `
    'app\argus\repro\Write-QtPackageObjectReceipt.ps1'
$temporaryRoot = Join-Path ([IO.Path]::GetTempPath()) `
    "argus-qt-package-receipt-$([Guid]::NewGuid().ToString('N'))"

try {
    $updatesXml = Join-Path $temporaryRoot 'Updates.xml'
    $archives = Join-Path $temporaryRoot 'archives'
    $contract = Join-Path $temporaryRoot 'contract.json'
    $receipt = Join-Path $temporaryRoot 'receipt.json'
    $archiveName = 'qtbase-fixture.7z'
    $objectName = "fixture-prefix-$archiveName"
    $archivePath = Join-Path $archives $objectName
    Write-Utf8File $archivePath 'fixture-archive-content'
    $length = (Get-Item -LiteralPath $archivePath).Length
    $sha1 = (Get-FileHash -Algorithm SHA1 -LiteralPath $archivePath).Hash
    $sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $archivePath).Hash
    Write-Utf8File "$archivePath.sha1" "$($sha1.ToLowerInvariant())  $objectName`n"
    Write-Utf8File $updatesXml @"
<?xml version="1.0" encoding="UTF-8"?>
<Updates>
  <PackageUpdate>
    <Name>qt.fixture</Name>
    <Version>fixture-prefix-</Version>
    <ReleaseDate>2026-08-05</ReleaseDate>
    <SHA1>0123456789abcdef0123456789abcdef01234567</SHA1>
    <DownloadableArchives>$archiveName</DownloadableArchives>
  </PackageUpdate>
</Updates>
"@
    $contractObject = [ordered]@{
        schemaVersion = 1
        algorithm = 'sha256-qt-package-object-set-v1'
        package = [ordered]@{
            name = 'qt.fixture'
            version = 'fixture-prefix-'
            releaseDate = '2026-08-05'
            sha1 = '0123456789ABCDEF0123456789ABCDEF01234567'
        }
        archives = @([ordered]@{
            metadataName = $archiveName
            length = $length
            sha1 = $sha1
            sha256 = $sha256
        })
    }
    Write-Utf8File $contract "$(ConvertTo-Json $contractObject -Depth 8)`n"

    & $receiptTool -UpdatesXmlPath $updatesXml -ArchiveDirectory $archives `
        -ContractPath $contract -OutputPath $receipt

    $result = Get-Content -Raw -LiteralPath $receipt | ConvertFrom-Json
    if ($result.schemaVersion -ne 1 -or
        $result.package.name -cne 'qt.fixture' -or
        $result.archives.Count -ne 1 -or
        $result.archives[0].objectName -cne $objectName -or
        $result.archives[0].sha256 -cne $sha256 -or
        $result.archives[0].officialSha1SidecarSha256 -cnotmatch '^[0-9A-F]{64}$') {
        throw 'The Qt package object receipt did not bind metadata to the verified object.'
    }

    Write-Utf8File $archivePath 'changed-vendor-content'
    try {
        & $receiptTool -UpdatesXmlPath $updatesXml -ArchiveDirectory $archives `
            -ContractPath $contract -OutputPath $receipt
        throw 'Changed vendor content was accepted.'
    }
    catch {
        if ($_.Exception.Message -notlike '*hash*') {
            throw
        }
    }

    Write-Output 'Qt package object receipt tests passed.'
}
finally {
    if (Test-Path -LiteralPath $temporaryRoot) {
        Remove-Item -LiteralPath $temporaryRoot -Recurse -Force
    }
}
