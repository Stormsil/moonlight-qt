# Reproducible Argus worker build

`Moonlight.exe` is the only canonical production worker artifact. The build
contract pins the complete task-local MSVC and Windows SDK include/lib/bin
trees, Qt and prebuilt dependency trees, repository build tools, source
ancestry, common-c base/patch/result tree, flags, timezone, language, and
`SOURCE_DATE_EPOCH`. The scripts reject dirty source authority, materialized
source drift, EOL drift, existing build roots, tool or input hash drift,
missing PE `REPRO` metadata, and embedded absolute input paths.

Materialize the clean authority twice, then run the two-build proof:

```powershell
app\argus\repro\Materialize-ReproducibleSource.ps1 `
  -SourceAuthorityRoot C:\task-owned\moonlight-qt `
  -Destination C:\task-owned\source-a
app\argus\repro\Materialize-ReproducibleSource.ps1 `
  -SourceAuthorityRoot C:\task-owned\moonlight-qt `
  -Destination C:\task-owned\source-b
app\argus\repro\Verify-ReproducibleWorker.ps1 `
  -BuildRootA C:\task-owned\argus-worker-a `
  -BuildRootB C:\task-owned\argus-worker-b `
  -SourceRootA C:\task-owned\source-a `
  -SourceRootB C:\task-owned\source-b `
  -SourceAuthorityRoot C:\task-owned\moonlight-qt `
  -QtRoot C:\task-owned\Qt\6.11.1\msvc2022_64 `
  -ToolchainRoot C:\task-owned\msvc-sdk `
  -ProofPath C:\task-owned\two-build-proof.json
```

The two source roots are independent materializations of the clean authority
tree, not extra Git owners. Each build receipt hashes its source-root identity,
complete materialized source tree, task-local toolchain trees, and artifact.
The proof embeds both atomic build receipts and their content hashes.

The MSVC PDB is retained as task-local diagnostic evidence only. MSVC records
task-local build paths in the PDB even with deterministic compiler path mapping,
so the PDB is hashed and audited but is not content-addressed, distributed, or
part of worker admission. `/PDBALTPATH:Moonlight.pdb` and `/Brepro` keep those
paths and wall-clock values out of `Moonlight.exe` itself.

This build does not pair with Sunshine, start a stream, install software, sign
or publish a package, or change ordinary interactive Moonlight behavior. GPL
distribution still requires the exact fork history, materialized patched
common-c tree, dependency sources/notices, build scripts, and a complete
Corresponding Source delivery alongside any conveyed binary.
