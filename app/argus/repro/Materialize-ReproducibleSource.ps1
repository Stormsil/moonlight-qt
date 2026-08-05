[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $SourceAuthorityRoot,

    [Parameter(Mandatory = $true)]
    [string] $Destination
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$authority = (Resolve-Path -LiteralPath $SourceAuthorityRoot).Path
$destinationPath = [IO.Path]::GetFullPath($Destination)
if (-not [IO.Path]::IsPathFullyQualified($Destination)) {
    throw 'Destination must be an absolute path.'
}
if (Test-Path -LiteralPath $destinationPath) {
    throw 'Destination must not exist.'
}

$repositoryRoot = (& git -C $authority rev-parse --show-toplevel).Trim()
if ($LASTEXITCODE -ne 0 -or
    [IO.Path]::GetFullPath($repositoryRoot) -cne
        [IO.Path]::GetFullPath($authority)) {
    throw 'SourceAuthorityRoot must be the Moonlight repository root.'
}
$status = @(& git -C $authority status --porcelain=v1 `
        --untracked-files=all --ignore-submodules=none)
if ($LASTEXITCODE -ne 0 -or $status.Count -ne 0) {
    throw 'The Moonlight source authority must be clean.'
}
$autoCrlf = (& git -C $authority config --get core.autocrlf).Trim()
if ($LASTEXITCODE -ne 0 -or $autoCrlf -cne 'false') {
    throw 'core.autocrlf must be false for source materialization.'
}

New-Item -ItemType Directory -Path $destinationPath | Out-Null
try {
    $pending = [Collections.Generic.Queue[string]]::new()
    $pending.Enqueue($authority)
    while ($pending.Count -gt 0) {
        $directory = $pending.Dequeue()
        $relativeDirectory = [IO.Path]::GetRelativePath(
            $authority,
            $directory)
        $targetDirectory = if ($relativeDirectory -ceq '.') {
            $destinationPath
        }
        else {
            Join-Path $destinationPath $relativeDirectory
        }
        New-Item -ItemType Directory -Force -Path $targetDirectory |
            Out-Null
        foreach ($entry in [IO.Directory]::EnumerateFileSystemEntries(
                $directory)) {
            if ([IO.Path]::GetFileName($entry) -ceq '.git') {
                continue
            }
            if ([IO.Directory]::Exists($entry)) {
                $pending.Enqueue($entry)
            }
            else {
                [IO.File]::Copy(
                    $entry,
                    (Join-Path $targetDirectory (
                            [IO.Path]::GetFileName($entry))),
                    $false)
            }
        }
    }
}
catch {
    if (Test-Path -LiteralPath $destinationPath) {
        Remove-Item -LiteralPath $destinationPath -Recurse -Force
    }
    throw
}

[ordered]@{
    sourceCommit = (& git -C $authority rev-parse HEAD).Trim()
    sourceTree = (& git -C $authority rev-parse 'HEAD^{tree}').Trim()
    destination = $destinationPath
} | ConvertTo-Json
