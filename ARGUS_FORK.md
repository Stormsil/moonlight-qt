# Argus worker fork

This branch is an opt-in integration spike for Bot-Mox issue 577. It preserves
Moonlight's GPL-3.0 license and upstream history. The fork remains a separate
GPL repository and is not vendored, copied, or linked into the Bot-Mox
repository.

## Upstream pin

- Upstream repository: `https://github.com/moonlight-stream/moonlight-qt`
- Upstream branch: `master`
- Upstream commit: `546cb72e32e5ac04bbc7e0b3a254176e5696685a`
- Fork branch: `argus/headless-worker-v1`

## Pristine build receipt

The pristine upstream pin was built before source changes on Windows x64:

- Qt: `6.11.1`, `msvc2022_64`
- Qt acquisition: `aqtinstall` commit
  `073e34d7c2ab4ae6961ed7cca690b3abd5ba5a7e`
- Toolchain: Visual Studio 2022 18.8
- Command: `scripts\build-arch.bat Release x64`
- Result: exit code 0, `Build successful for Moonlight v546cb7 x64 binaries`

Qt, aqt, and the standalone packaging tool were installed in a task-owned
directory. No global PATH or Visual Studio installation was changed.

## Current worker slice

The exact arguments `--argus-worker --protocol 1` select an early,
non-interactive bootstrap before Moonlight initializes paths, logging, SDL,
Qt GUI, settings, or identity state. All other startup paths continue through
the existing entry point.

Worker startup accepts only an inherited `ARGUS_STREAM_STARTUP_PIPE` capability.
It removes that variable immediately, rejects endpoint material and all
unrecognized arguments, validates the bounded pipe name, and fails closed when
the capability or pipe is unavailable. The current slice only proves argument
routing and connection to the inherited pipe. It deliberately exits before
claiming an authenticated handshake, protected identity delivery, streaming,
pairing, first-frame, or performance support.

The focused bootstrap test is built and run with:

```powershell
qmake tests\argus-worker-bootstrap\argus-worker-bootstrap.pro `
  -o build\argus-worker-bootstrap\Makefile
Push-Location build\argus-worker-bootstrap
nmake /f Makefile.Release
.\release\argus-worker-bootstrap-tests.exe
Pop-Location
```

## Reuse seams

- Pairing must remain on the existing
  `CliPair -> ComputerManager -> NvPairingManager -> IdentityManager` path.
- A future worker frame handoff should attach after
  `avcodec_receive_frame()` in `FFmpegVideoDecoder::decoderThreadProc()` and
  before `m_Pacer->submitFrame(frame)`.
- No second GameStream client or parallel identity store is permitted.

## Remaining gates

This spike is not release-ready. It still requires the authenticated startup
protocol, Machine-bound identity ingestion, decoded-frame transport, real
Sunshine pairing/streaming/first-frame evidence, 1920x1080 performance evidence,
and GPL/legal approval for the eventual distribution boundary. No release or
package is published from this branch.
