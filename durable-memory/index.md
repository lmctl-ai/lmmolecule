# lmmolecule durable memory

`lmmolecule` is a static, dependency-free lmscript `_dsl_` host embedding OpenMM 8.2.0 and Lepton for molecular dynamics, energy minimization, force fields, and PDB/trajectory analysis.
Repository: https://github.com/lmctl-ai/lmmolecule.

## 60-second takeover

```sh
cmake -S . -B build                   # Configure CMake build
cmake --build build --parallel 4      # Build binary at build/lmmolecule
./test.sh                             # Run verification test suite (physics oracles, examples)
```

## Document map

| File | Contents |
| --- | --- |
| [01-purpose-and-place.md](01-purpose-and-place.md) | Scope, OpenMM embedding, and simulation host capabilities |
| [02-libraries-and-licenses.md](02-libraries-and-licenses.md) | OpenMM 8.2.0, licensing audit, static build configuration |
| [03-schema-v0.md](03-schema-v0.md) | `md`, `System`, and `Sim` APIs, unit system conventions, and schema |
| [04-expressions.md](04-expressions.md) | Lepton custom potential expressions and evaluation |
| [05-build-run-test.md](05-build-run-test.md) | Build commands, test.sh checks, and determinism benchmarks |
| [06-ci.md](06-ci.md) | GitHub Actions CI matrix (5 platforms) and build workflows |
| [07-examples.md](07-examples.md) | Molecular dynamics examples and physical validation oracles |
| [08-open-items.md](08-open-items.md) | Backlog, missing forcefield features, and roadmap |
| [09-resource-limits.md](09-resource-limits.md) | Memory budget allocation (`LMMOLECULE_MAX_MEMORY_MB`) and particle caps |
| [next-release-version.md](next-release-version.md) | Version alignment protocol (VERSION, CMake, THIRD_PARTY_NOTICES) |

## SDK pin and sync

The vendored SDK header is pinned via `sdk.lock` pointing to `https://github.com/lmctl-ai/lmcode-cpp`:
```sh
scripts/sync-sdk.sh                   # Refetches pinned cpp/dslvm.hpp and verifies sha256
```

## Adding an example or function

### Adding an example
1. Create `examples/<name>.lms` using `_dsl_ cli="lmmolecule" install="github:lmctl-ai/lmmolecule@latest"`.
2. Run script: `build/lmmolecule run examples/<name>.lms` or via `lmscript`.
3. Add the example to `./test.sh` test run list.
4. Save visual output to `docs/images/<name>.png` and update `README.md` demo table with `lmctl autorun` command.

### Adding a host function
1. Declare function/method in `schema/lmmolecule.dsl.json`.
2. Register and implement the function in C++ source under `src/` using `dslvm::Registry`.
3. Rebuild and verify schema synchronization with `cmake --build build` and `./test.sh`.

## Releases

Releases are triggered automatically on push of a `v*` tag. The release workflow builds across Linux x64/arm64, macOS x64/arm64, and Windows x64, publishing platform binaries with `SHA256SUMS`, `LICENSE`, and `THIRD_PARTY_NOTICES`. See `next-release-version.md` before tagging.

Compiler >=0.2.420 resolves std from the pinned SDK bundle by default; host schema bytes remain host-only, with full and `--without-std` parity plus a standard-only runtime probe in `test.sh`.
