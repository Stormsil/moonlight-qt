# Reproducible Argus worker build

`Moonlight.exe` is the only canonical production worker artifact. The build
contract pins the complete Qt and prebuilt dependency trees, compiler/linker
and Windows SDK binaries, repository build tools, source ancestry, common-c
base/patch/result tree, flags, timezone, language, and `SOURCE_DATE_EPOCH`.
The scripts reject dirty source, EOL drift, existing build roots, tool or input
hash drift, missing PE `REPRO` metadata, and embedded absolute input paths.

Run the two-build proof from a clean `core.autocrlf=false` checkout:

```powershell
app\argus\repro\Verify-ReproducibleWorker.ps1 `
  -BuildRootA C:\task-owned\argus-worker-a `
  -BuildRootB C:\task-owned\argus-worker-b `
  -QtRoot C:\task-owned\Qt\6.11.1\msvc2022_64 `
  -ProofPath C:\task-owned\two-build-proof.json
```

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
