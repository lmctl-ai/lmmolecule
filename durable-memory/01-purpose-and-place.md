# Purpose and place

lmmolecule is the molecular dynamics host of the *scripted scientific libraries* family (see
`../../simulation/ROADMAP.md`): OpenMM 8 (Core API, Reference and SIMD CPU platforms, Lepton
expression compiler, PocketFFT 3D Particle Mesh Ewald, SFMT random number generator, and L-BFGS
local energy minimizer) statically linked into one executable, driven from lmscript `_dsl_`
bytecode through an embedded schema.

- **Scripting only.** There is no user-facing command line beyond `--dsl`, `--dry-run`,
  `--version`, `--notices`, `--dsl-schema [--json]` and `--dsl-signatures` (the conventions
  of the sibling hosts).
- **Libraries**: OpenMM 8.2.0 (`openmmapi`, `olla`, `platforms/reference`, `platforms/cpu`,
  `libraries/lepton`, `libraries/pocketfft`, `libraries/sfmt`, `libraries/lbfgs`,
  `libraries/hilbert`, `libraries/vecmath`, `libraries/irrxml`, `libraries/quern`,
  `libraries/jama`, `serialization`). Pinned in `deps.lock` and fetched into git-ignored
  `third_party/openmm` by `scripts/sync-deps.sh`; nothing of OpenMM is committed.
- **cpp-dsl-sdk** (`cpp/dslvm.hpp`, bytecode v5) is vendored and pinned by `sdk.lock`.
- **lmscript** binds programs to the binary with
  `_dsl_ cli="lmmolecule" install="github:lmctl-ai/lmmolecule@latest";` so `lmscript prog.lms`
  is all a user runs; the schema is read from the binary (`lmmolecule --dsl-schema --json`).

## Scope boundary

The DSL has no lists, maps or closures. Therefore:

- particle systems and simulations are host objects (`System` and `Sim`, with builder-style
  methods returning the receiver);
- custom external fields, custom bonds, and custom pair potentials are Lepton expression strings
  compiled and analytically differentiated by OpenMM's Lepton library;
- trajectories and thermodynamic series are streamed to space-separated CSV (`Sim.report`) and
  multi-frame XYZ (`Sim.xyz`, `Sim.save_xyz`, `System.load_xyz`) for interchange with other
  hosts (`lmplot` draws from CSV).

Out of scope for v0: GPU platforms (CUDA, OpenCL, HIP), AMBER/CHARMM XML force-field parameter
loaders, implicit solvent (GBSA/OBC), alchemical free-energy plugins, and multi-time-step rRESPA.
