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
the capability or pipe is unavailable.

The native `StartupChannel` implements version 1 of the single managed contract
in Bot-Mox `StreamWorkerStartupHandshake.cs`: bounded little-endian
length-prefixed packets, the 32-byte challenge nonce, opaque
`.NET Guid.ToByteArray()` session fields, and an exact PID plus Windows process
creation-time hello. Endpoint, identity bytes, and the required frame-slot
descriptor are read only after the managed server accepts that hello. The
channel is single-use, has one overall I/O deadline, performs no retry or
downgrade, rejects malformed/truncated/trailing/out-of-bounds packets, and
zeroes temporary packet, nonce, endpoint, identity, and capability buffers.

The only supported identity format is `moonlight-qt.identity-v1`. Its package
is bounded to 256 KiB and uses this canonical little-endian layout:

- 8 bytes ASCII magic `MLQTIDPK`;
- `int32` version `1`, then `uint16` field count `3`;
- field `uint16 1` + `int32` length + 1..16 lowercase hexadecimal unique-ID
  bytes;
- field `uint16 2` + `int32` length + certificate PEM bytes;
- field `uint16 3` + `int32` length + private-key PEM bytes.

The decoder rejects unsupported format/version, reordered or duplicate fields,
empty/overflowing/out-of-bounds fields, trailing bytes, noncanonical IDs,
malformed or non-RSA PEM, multiple/unsupported/trailing PEM objects, and
certificate/private-key mismatch. Each field contains exactly one supported
PEM object with only surrounding whitespace. Accepted material moves into the
existing `IdentityManager` through a one-shot process-local install before its
first `get()`. The existing getters and SSL configuration then use that
material. This path never constructs `QSettings`;
second and post-`get()` installs fail closed. Ordinary pairing and the normal
lazy `QSettings` identity constructor are unchanged.

The focused bootstrap test is built and run with:

```powershell
qmake tests\argus-worker-bootstrap\argus-worker-bootstrap.pro `
  -o build\argus-worker-bootstrap\Makefile
Push-Location build\argus-worker-bootstrap
nmake /f Makefile.Release
.\release\argus-worker-bootstrap-tests.exe
Pop-Location
```

The managed Bot-Mox harness additionally launches the built `Moonlight.exe` as
the real child with runtime-generated task-owned RSA credentials. It proves a
valid package reaches `IdentityManager`, wrong PID and exact creation-time
receipts are rejected by the managed server, and wrong format, malformed
package, and mismatched keys fail closed in the child. Stdout/stderr remain
empty and task-owned working directories remain clean. This is startup and
identity-ingestion evidence only, not a production readiness or streaming
receipt.

`--argus-worker --protocol 1 --pairing-control` retains that authenticated
pipe after startup and accepts one session-bound pairing request. Pair mode
uses the existing `NvPairingManager` protocol with a four-digit PIN supplied
only through the pipe, then returns the resulting exact
`moonlight-qt.identity-v1` package and Sunshine certificate on the same
channel. Verify mode discovers only the advertised HTTPS port over HTTP and
then performs the paired-state request directly over pinned HTTPS; it never
uses the ordinary helper's HTTP fallback. Wrong PIN, unavailable host,
certificate mismatch, malformed control, duplicate exchange, and stale
session all fail closed without retry. Ordinary interactive pairing remains
unchanged.

The task-owned Windows oracle paired a fresh generated identity with unmodified
Sunshine `2026.516.143833`, persisted the identity/endpoint/server-certificate
binding through the Bot-Mox protected-store port, restarted Sunshine, and
verified the same pairing from a second fresh worker without another PIN. It
also proved wrong-PIN, unavailable-host, wrong-certificate, listener-ownership,
empty worker-output, and cleanup gates. This is pairing evidence only; it does
not claim a stream or decoded frame.

## Reuse seams

- Worker pairing reuses the existing `NvPairingManager -> IdentityManager`
  protocol/identity core; ordinary `CliPair -> ComputerManager` behavior stays
  unchanged and no second GameStream client is introduced.
- A future worker frame handoff should attach after
  `avcodec_receive_frame()` in `FFmpegVideoDecoder::decoderThreadProc()` and
  before `m_Pacer->submitFrame(frame)`.
- No second GameStream client or parallel identity store is permitted.

## Remaining gates

This spike is not release-ready. It still requires real Sunshine
connect/first-frame/disconnect evidence, decoded frame transport, 1920x1080
performance evidence, packaging/SBOM/corresponding-source work, and GPL/legal
approval for the eventual distribution boundary. No release or package is
published from this branch.
