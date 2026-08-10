param(
    [string] $RepositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..'))
)

$ErrorActionPreference = 'Stop'

function Get-CppFunctionBody {
    param(
        [Parameter(Mandatory)]
        [string] $Source,
        [Parameter(Mandatory)]
        [string] $Signature
    )

    $signatureIndex = $Source.IndexOf(
        $Signature,
        [System.StringComparison]::Ordinal)
    if ($signatureIndex -lt 0) {
        throw "Function signature not found: $Signature"
    }

    $bodyStart = $Source.IndexOf('{', $signatureIndex)
    if ($bodyStart -lt 0) {
        throw "Function body not found: $Signature"
    }

    $depth = 0
    for ($index = $bodyStart; $index -lt $Source.Length; $index++) {
        switch ($Source[$index]) {
            '{' { $depth++ }
            '}' {
                $depth--
                if ($depth -eq 0) {
                    return $Source.Substring(
                        $bodyStart,
                        $index - $bodyStart + 1)
                }
            }
        }
    }

    throw "Unterminated function body: $Signature"
}

$sessionPath = Join-Path $RepositoryRoot 'app\streaming\session.cpp'
$sessionSource = Get-Content -LiteralPath $sessionPath -Raw
$workerRun = Get-CppFunctionBody `
    -Source $sessionSource `
    -Signature 'Session::ArgusHeadlessOutcome Session::runArgusHeadless('
$workerInitialization = Get-CppFunctionBody `
    -Source $sessionSource `
    -Signature 'bool Session::initializeArgusHeadless()'
$interactiveInitialization = Get-CppFunctionBody `
    -Source $sessionSource `
    -Signature 'bool Session::initialize(QQuickWindow* qtWindow)'
$streamClientPath = Join-Path $RepositoryRoot 'app\argus\streamclient.cpp'
$streamClientSource = Get-Content -LiteralPath $streamClientPath -Raw
$streamControl = Get-CppFunctionBody `
    -Source $streamClientSource `
    -Signature 'StreamControlOutcome executeStreamControl('
$oracleRunnerPath = Join-Path $RepositoryRoot `
    'tests\argus-renderer-free-worker\Run-RendererFreeWorkerOracle.ps1'
$oracleRunnerSource = Get-Content -LiteralPath $oracleRunnerPath -Raw

if (-not $oracleRunnerSource.Contains(
        '$env:SDL_RENDER_DRIVER = ''software''',
        [System.StringComparison]::Ordinal) -or
    -not $oracleRunnerSource.Contains(
        '$env:SDL_RENDER_DRIVER = $priorSdlRenderDriver',
        [System.StringComparison]::Ordinal)) {
    throw 'Renderer-free oracle does not pin and restore the software SDL renderer.'
}

if ($workerRun.Contains(
        'SDL_CreateWindow',
        [System.StringComparison]::Ordinal)) {
    throw 'Explicit Argus worker constructs an SDL window before first-frame publication.'
}

if ($workerInitialization.Contains(
        'initialize(nullptr)',
        [System.StringComparison]::Ordinal)) {
    throw 'Explicit Argus worker delegates to interactive SDL/renderer initialization.'
}

if (-not $workerRun.Contains(
        'activeDecodedFrameFailureCode()',
        [System.StringComparison]::Ordinal) -or
    -not $workerRun.Contains(
        'ArgusHeadlessOutcome::SinkFailed',
        [System.StringComparison]::Ordinal)) {
    throw 'Explicit Argus worker does not terminate promptly on typed sink publication failure.'
}

if (-not $interactiveInitialization.Contains(
        'SDL_InitSubSystem(SDL_INIT_VIDEO)',
        [System.StringComparison]::Ordinal) -or
    -not $interactiveInitialization.Contains(
        'StreamUtils::createTestWindow()',
        [System.StringComparison]::Ordinal)) {
    throw 'Ordinary interactive initialization no longer constructs its decoder test window.'
}

$requiredStreamControlCalls = @(
    'sendFrameReady('
    'receiveStreamCommand('
    'acceptFrameConsumed('
    'acknowledgeConsumed('
    'waitForFrameOrControl('
    'stopArgusHeadless()')
foreach ($requiredCall in $requiredStreamControlCalls) {
    if (-not $streamControl.Contains(
            $requiredCall,
            [System.StringComparison]::Ordinal)) {
        throw "Argus multi-frame stream control is missing: $requiredCall"
    }
}
if ($streamControl.Contains(
        'PltSleepMs(',
        [System.StringComparison]::Ordinal) -or
    $streamControl.Contains(
        'QThread::msleep(',
        [System.StringComparison]::Ordinal)) {
    throw 'Argus multi-frame stream control regressed to polling.'
}

$sdlRendererPath = Join-Path $RepositoryRoot `
    'app\streaming\video\ffmpeg-renderers\sdlvid.cpp'
$sdlRendererSource = Get-Content -LiteralPath $sdlRendererPath -Raw
$sdlRendererInitialization = Get-CppFunctionBody `
    -Source $sdlRendererSource `
    -Signature 'bool SdlRenderer::initialize(PDECODER_PARAMETERS params)'
if (-not $sdlRendererInitialization.Contains(
        'SDL_CreateRenderer(params->window',
        [System.StringComparison]::Ordinal)) {
    throw 'Ordinary interactive SDL renderer construction is no longer preserved.'
}

$preferencesPath = Join-Path $RepositoryRoot `
    'app\settings\streamingpreferences.cpp'
$preferencesSource = Get-Content -LiteralPath $preferencesPath -Raw
$workerPreferences = Get-CppFunctionBody `
    -Source $preferencesSource `
    -Signature 'StreamingPreferences* StreamingPreferences::createArgusWorker('
if (-not $workerPreferences.Contains(
        'videoCodecConfig = VCC_FORCE_H264',
        [System.StringComparison]::Ordinal) -or
    -not $workerPreferences.Contains(
        'videoDecoderSelection = VDS_FORCE_SOFTWARE',
        [System.StringComparison]::Ordinal)) {
    throw 'Argus worker codec/decoder contract drifted from its explicit H.264 software profile.'
}

Write-Output 'Renderer-free Argus worker source contract passed.'
