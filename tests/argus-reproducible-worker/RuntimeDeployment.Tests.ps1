[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Assert-Equal([object] $Expected, [object] $Actual, [string] $Message) {
    if ($Expected -cne $Actual) {
        throw "$Message Expected '$Expected', got '$Actual'."
    }
}

$sourceRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$contractPath = Join-Path $sourceRoot `
    'app\argus\repro\runtime-deployment-v1.json'
$contract = Get-Content -Raw -LiteralPath $contractPath | ConvertFrom-Json

Assert-Equal 1 $contract.schemaVersion 'Runtime schema drifted.'
Assert-Equal 6 $contract.manifestSchemaVersion `
    'Runtime manifest schema drifted.'
Assert-Equal '014dc6fff6ed7790c6ff411cec05cbd709d3bad9' `
    $contract.requiredAncestor 'Producer ancestry drifted.'
Assert-Equal 'C0CACE23418F88F8DABD53EB1B4810C68FE10A4C055400D8BA59360B15B92385' `
    $contract.worker.sha256 'Worker identity drifted.'
Assert-Equal 2832384 $contract.worker.length 'Worker length drifted.'
Assert-Equal 'B795CF0072ABA8E313B0F127B01E48B1C328A81D03CBA8B1B121D18420934D14' `
    $contract.antiHooking.sha256 'AntiHooking identity drifted.'
Assert-Equal 47104 $contract.antiHooking.length 'AntiHooking length drifted.'
Assert-Equal 'B8272265B99CCEE3227C4B01E12517482DC913FC91A23A3BB609961FF3696FD8' `
    $contract.qt.treeSha256 'Qt identity drifted.'
Assert-Equal '195EFB527A1E3B66EC3A36DFA6D4057FC86384873F957B0A04C72854F1BE8E2E' `
    $contract.dependencies.treeSha256 'Dependency identity drifted.'
Assert-Equal '3BE53DEB7B9AB371372FF676C31655CF552CE4040F90423E8293E0E067A619E6' `
    $contract.qt.windeployqtSha256 'windeployqt identity drifted.'
Assert-Equal 'D0ED426B928FCF322254017822EDD74EDAB6014772CA5098F2583782AC8D965D' `
    $contract.peImportTool.sha256 'PE import tool identity drifted.'
Assert-Equal 'bin' $contract.qt.peImportClosureSourceRelativePath `
    'PE import closure source drifted.'

$expectedArguments = @(
    '--dir={runtimeRoot}', '--release', '--qmldir={sourceRoot}/app/gui',
    '--no-opengl-sw', '--no-compiler-runtime', '--no-sql',
    '--no-system-d3d-compiler', '--no-system-dxc-compiler',
    '--skip-plugin-types=qmltooling,generic', '--no-ffmpeg',
    '--no-quickcontrols2fusion', '--no-quickcontrols2imagine',
    '--no-quickcontrols2universal', '--no-quickcontrols2fusionstyleimpl',
    '--no-quickcontrols2imaginestyleimpl',
    '--no-quickcontrols2universalstyleimpl',
    '--no-quickcontrols2windowsstyleimpl',
    '--no-quickcontrols2fluentwinui3styleimpl')
Assert-Equal ($expectedArguments -join "`n") `
    (@($contract.qt.arguments) -join "`n") `
    'windeployqt invocation drifted.'

foreach ($required in @(
        'AntiHooking.dll', 'gamecontrollerdb.txt',
        'platforms/qwindows.dll', 'tls/qcertonlybackend.dll',
        'tls/qopensslbackend.dll',
        'tls/qschannelbackend.dll')) {
    if (@($contract.qt.requiredRuntimePaths) -cnotcontains $required) {
        throw "Required runtime path '$required' is unbound."
    }
}
$opensslRule = @($contract.qt.postCopyRules | Where-Object {
    $_.targetRelativePath -ceq 'tls/qopensslbackend.dll'
})
Assert-Equal 1 $opensslRule.Count 'OpenSSL TLS plugin copy rule drifted.'
Assert-Equal 'BD6C98ABD328149E7E3CFA651753AB8654F404C9D63ACED7006B3863E6F5AA26' `
    $opensslRule[0].sha256 'OpenSSL TLS plugin identity drifted.'
if ($contract.ambientPathAllowed -ne $false -or
    $contract.distributionAllowed -ne $false -or
    $contract.legalApproval -ne $false) {
    throw 'Runtime safety/legal gates must remain false.'
}

$buildScript = Get-Content -Raw -LiteralPath (
    Join-Path $sourceRoot 'app\argus\repro\Build-ReproducibleWorker.ps1')
if ($buildScript -cnotmatch 'LDFLAGS=.*PDBALTPATH:AntiHooking\.pdb' -or
    $buildScript -cnotmatch '\$antiHook = Join-Path \$artifactRoot') {
    throw 'The independent reproducible AntiHooking.dll build is missing.'
}
$materializer = Get-Content -Raw -LiteralPath (Join-Path $sourceRoot `
    'app\argus\repro\Materialize-RuntimeDeployment.ps1')
if ($materializer -cnotmatch 'source authority changed during materialization') {
    throw 'Runtime materialization does not revalidate producer authority.'
}

Write-Output 'Runtime deployment contract tests passed.'
