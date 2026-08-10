param(
    [Parameter(Mandatory)]
    [string] $OracleExecutable,
    [Parameter(Mandatory)]
    [string] $WorkerArtifact,
    [Parameter(Mandatory)]
    [string] $RuntimeRoot,
    [Parameter(Mandatory)]
    [string] $OutputPath,
    [Parameter(Mandatory)]
    [string] $SourceRoot,
    [Parameter(Mandatory)]
    [string] $ArtifactSourceCommit,
    [Parameter(Mandatory)]
    [string] $ExpectedSourceCommit
)

$ErrorActionPreference = 'Stop'
$RunnerRelativePath =
    'tests/argus-renderer-free-worker/Run-RendererFreeWorkerOracle.ps1'
$OracleConfiguration = 'release-x64-argus_renderer_free_oracle'

function Get-Sha256 {
    param([Parameter(Mandatory)][string] $Path)
    return (Get-FileHash -Algorithm SHA256 -LiteralPath $Path).Hash
}

function Get-GitBlobSha256 {
    param(
        [Parameter(Mandatory)][string] $Repository,
        [Parameter(Mandatory)][string] $Commit,
        [Parameter(Mandatory)][string] $RelativePath
    )

    $start = [System.Diagnostics.ProcessStartInfo]::new('git')
    $start.UseShellExecute = $false
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $start.ArgumentList.Add('-C')
    $start.ArgumentList.Add($Repository)
    $start.ArgumentList.Add('show')
    $start.ArgumentList.Add("$Commit`:$RelativePath")
    $process = [System.Diagnostics.Process]::Start($start)
    try {
        $bytes = [System.IO.MemoryStream]::new()
        try {
            $process.StandardOutput.BaseStream.CopyTo($bytes)
            $errorText = $process.StandardError.ReadToEnd()
            $process.WaitForExit()
            if ($process.ExitCode -ne 0) {
                throw "git show failed: $errorText"
            }
            return [Convert]::ToHexString(
                [Security.Cryptography.SHA256]::HashData($bytes.ToArray()))
        }
        finally {
            $bytes.Dispose()
        }
    }
    finally {
        $process.Dispose()
    }
}

$oracle = (Resolve-Path -LiteralPath $OracleExecutable).Path
$worker = (Resolve-Path -LiteralPath $WorkerArtifact).Path
$runtime = (Resolve-Path -LiteralPath $RuntimeRoot).Path
$source = (Resolve-Path -LiteralPath $SourceRoot).Path
$output = [IO.Path]::GetFullPath($OutputPath)
$runner = (Resolve-Path -LiteralPath $PSCommandPath).Path
if (Test-Path -LiteralPath $output) {
    throw "Oracle output already exists: $output"
}

$head = (git -C $source rev-parse HEAD).Trim()
$status = git -C $source status --porcelain=v1 --untracked-files=all
$sourceInvalid = $LASTEXITCODE -ne 0 `
    -or $head -ne $ExpectedSourceCommit `
    -or -not [string]::IsNullOrWhiteSpace($status)
if ($sourceInvalid) {
    throw 'Oracle source must be clean at the exact runner source commit.'
}
$artifactAncestor = git -C $source merge-base --is-ancestor `
    $ArtifactSourceCommit $head
if ($LASTEXITCODE -ne 0) {
    throw 'Artifact source commit must be an ancestor of the oracle runner.'
}
$tree = (git -C $source rev-parse "$ArtifactSourceCommit`^{tree}").Trim()
if ($ArtifactSourceCommit -notmatch '^[0-9a-f]{40}$' -or
    $tree -notmatch '^[0-9a-f]{40}$') {
    throw 'Artifact source authority is invalid.'
}

$artifactSha256 = Get-Sha256 $worker
$artifactLength = (Get-Item -LiteralPath $worker).Length
$oracleSha256 = Get-Sha256 $oracle
$oracleLength = (Get-Item -LiteralPath $oracle).Length
$runnerWorkingTreeSha256 = Get-Sha256 $runner
$runnerGitBlobSha256 = Get-GitBlobSha256 `
    -Repository $source `
    -Commit $head `
    -RelativePath $RunnerRelativePath

$priorPath = $env:PATH
$priorQtPluginPath = $env:QT_PLUGIN_PATH
$priorSdlRenderDriver = $env:SDL_RENDER_DRIVER
try {
    $env:PATH = "$runtime;C:\Windows\System32;C:\Windows"
    $env:QT_PLUGIN_PATH = $runtime
    $env:SDL_RENDER_DRIVER = 'software'
    $arguments = @(
        '--argus-renderer-free-oracle',
        $output,
        $artifactSha256,
        [string]$artifactLength,
        $ArtifactSourceCommit,
        $tree,
        $oracleSha256,
        [string]$oracleLength,
        $RunnerRelativePath,
        $runnerWorkingTreeSha256,
        $runnerGitBlobSha256,
        $head)
    $process = Start-Process `
        -FilePath $oracle `
        -ArgumentList $arguments `
        -WorkingDirectory ([IO.Path]::GetDirectoryName($oracle)) `
        -WindowStyle Hidden `
        -Wait `
        -PassThru
    if ($process.ExitCode -ne 0) {
        throw "Renderer-free runtime oracle failed with exit $($process.ExitCode)."
    }
}
finally {
    $env:PATH = $priorPath
    $env:QT_PLUGIN_PATH = $priorQtPluginPath
    $env:SDL_RENDER_DRIVER = $priorSdlRenderDriver
}

if (-not (Test-Path -LiteralPath $output)) {
    throw 'Renderer-free runtime oracle did not create its receipt.'
}
$receipt = Get-Content -LiteralPath $output -Raw | ConvertFrom-Json
$bindingChecks = @(
    $receipt.passed
    $receipt.schemaVersion -eq 3
    $receipt.authority.artifact.sha256 -eq $artifactSha256
    $receipt.authority.artifact.length -eq $artifactLength
    $receipt.authority.artifact.sourceCommit -eq $ArtifactSourceCommit
    $receipt.authority.artifact.sourceTree -eq $tree
    $receipt.authority.oracle.executableSha256 -eq $oracleSha256
    $receipt.authority.oracle.executableLength -eq $oracleLength
    $receipt.authority.oracle.configuration -eq $OracleConfiguration
    $receipt.authority.oracle.runnerPath -eq $RunnerRelativePath
    $receipt.authority.oracle.runnerWorkingTreeSha256 `
        -eq $runnerWorkingTreeSha256
    $receipt.authority.oracle.runnerGitBlobSha256 -eq $runnerGitBlobSha256
    $receipt.authority.oracle.runnerCommit -eq $head)
if ($bindingChecks -contains $false) {
    throw 'Renderer-free runtime receipt authority binding is invalid.'
}

Get-FileHash -Algorithm SHA256 -LiteralPath $output
