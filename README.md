# lmmolecule

Molecular dynamics with OpenMM, scripted in lmscript and shown through reproducible experiments.

| Argon crystal | Ionic lattice | Butane relaxation |
| --- | --- | --- |
| ![Minimized FCC argon coordinates](docs/images/argon_structure.png) | ![Rock-salt sodium and chloride sites](docs/images/nacl_structure.png) | ![Butane before and after minimization](docs/images/butane_relaxation.png) |
| [32 argon sites after minimization](examples/visual/argon_structure.lms)<br>`lmctl autorun https://raw.githubusercontent.com/lmctl-ai/lmmolecule/main/examples/visual/argon_structure.lms`. | [Na+ blue, Cl- green; rock-salt coordinates](examples/visual/nacl_structure.lms)<br>`lmctl autorun https://raw.githubusercontent.com/lmctl-ai/lmmolecule/main/examples/visual/nacl_structure.lms`. | [Orange initial, teal minimized; four united-atom sites](examples/visual/butane_relaxation.lms)<br>`lmctl autorun https://raw.githubusercontent.com/lmctl-ai/lmmolecule/main/examples/visual/butane_relaxation.lms`. |

| Attraction and repulsion | Mass changes vibration | Anharmonic motion |
| --- | --- | --- |
| ![Lennard-Jones binding curve](docs/images/lj_binding.png) | ![Light and heavy dimer bond distances](docs/images/isotope_oscillation.png) | ![Animated Morse dimer](docs/images/morse-motion.gif) |
| [The Lennard-Jones binding well](examples/visual/lj_binding.lms)<br>`lmctl autorun https://raw.githubusercontent.com/lmctl-ai/lmmolecule/main/examples/visual/lj_binding.lms`. | [A heavier dimer oscillates more slowly](examples/visual/isotope_oscillation.lms)<br>`lmctl autorun https://raw.githubusercontent.com/lmctl-ai/lmmolecule/main/examples/visual/isotope_oscillation.lms`. | [A stretched Morse bond exchanges energy](examples/visual/morse_motion.lms)<br>`lmctl autorun https://raw.githubusercontent.com/lmctl-ai/lmmolecule/main/examples/visual/morse_motion.lms`. |

## Install and run

Install [lmscript](https://lmctl.com/tools/lmscript/skill.html), then run an example from this repository root:

```sh
lmscript examples/visual/lj_binding.lms
# Rebuild all images (requires the sibling hosts and ffmpeg for animation):
LMMOLECULE_TEST_NETWORK=1 examples/render-all.sh
```

The `install=` bindings download released hosts on first use; local binaries on `PATH` take priority.
See the [complete image gallery](docs/gallery.md) for every experiment, including water bending and thermal trapping.

```
# argon-fcc.lms
p = _dsl_ cli="lmmolecule" install="github:lmctl-ai/lmmolecule@latest";
let sys = md.system().fcc(3, 0.54, 39.948).lj(0.996, 0.3405, 0.78);
let sim = sys.sim("langevin", 0.002, 120.0, 5.0, 42);
sim.minimize(0.001, 200)
   .velocities(120.0, 42)
   .report("argon.csv", 50)
   .xyz("argon.xyz", 200)
   .step(800);
end _dsl_
savedsl(p, "argon.dslb");
system ${dsl_cli(p)} --dsl argon.dslb;
```

`lmscript argon-fcc.lms` finds `lmmolecule` on `PATH` or downloads the release binary for your
platform on first use, compiles against the schema built into it, and runs the program.

## What it does

Units follow OpenMM's standard molecular dynamics convention (`nm`, `ps`, `amu` / `g/mol`,
elementary charge `e`, `K`, `kJ/mol`, radians, `bar`). User-supplied potential energy expressions
are compiled by OpenMM's embedded Lepton engine; see
[durable-memory/04-expressions.md](durable-memory/04-expressions.md).

| `md.` / `System` / `Sim` | |
| --- | --- |
| `md.system()` `md.platform("reference"\|"cpu")` | create system builder; select Reference or SIMD CPU platform |
| `sys.box` `particle` `add_particle` `pos` `vel` `count` | periodic box and explicit particles |
| `sys.fcc` `rocksalt` `gas` `load_xyz` | crystal lattices (FCC, NaCl rock-salt), seeded gas, XYZ loader |
| `sys.bond` `angle` `torsion` | harmonic bonds, harmonic angles, periodic dihedral torsions |
| `sys.lj` `lj_particle` `charge` `coulomb` `exclude` | Lennard-Jones 12-6, Coulomb (`none`/`cutoff`/`ewald`/`pme`), exclusions |
| `sys.custom_external` `custom_bond` `custom_nonbonded` | Lepton custom external traps, custom bonds, custom pair potentials |
| `sys.barostat` `sys.sim("verlet"\|"langevin", ...)` | Monte Carlo NPT barostat; Verlet or BAOAB Langevin middle integrator |
| `sim.minimize` `velocities` `step` `report` `xyz` `save_xyz` | L-BFGS minimization, Maxwell-Boltzmann seeding, CSV/XYZ reporters |
| `sim.ke` `pe` `energy` `temperature` `time` `distance` `angle_val` `box_volume` | thermodynamic observables and geometry queries |
| `md.eval(expr)` `assert_close` `print` `version` | physical constants (`pi`, `e`, `kB`, `one_4pi_eps0`), assertions, output |

Schema and rules: [durable-memory/03-schema-v0.md](durable-memory/03-schema-v0.md).

Combine it with other hosts in one script: [examples/combo/morse_plot.lms](examples/combo/morse_plot.lms)<br>`lmctl autorun https://raw.githubusercontent.com/lmctl-ai/lmmolecule/main/examples/combo/morse_plot.lms`
integrates an anharmonic Morse diatomic oscillator with `lmmolecule` and plots kinetic, potential,
and total energy with [lmplot](https://github.com/lmctl-ai/lmplot) (two `_dsl_` blocks, two hosts).

![Anharmonic Morse diatomic oscillator](docs/images/morse_plot.png)

## Examples

Eight new [visual experiments](docs/gallery.md) supplement the original simulations below.

| Program | Shows |
| --- | --- |
| [argon_fcc_melt.lms](examples/argon_fcc_melt.lms)<br>`lmctl autorun https://raw.githubusercontent.com/lmctl-ai/lmmolecule/main/examples/argon_fcc_melt.lms` | 108-atom FCC argon crystal L-BFGS minimization + Langevin equilibration |
| [nacl_madelung.lms](examples/nacl_madelung.lms)<br>`lmctl autorun https://raw.githubusercontent.com/lmctl-ai/lmmolecule/main/examples/nacl_madelung.lms` | 512-ion NaCl rock-salt lattice PME electrostatics → Madelung constant 1.747565 |
| [butane_torsion.lms](examples/butane_torsion.lms)<br>`lmctl autorun https://raw.githubusercontent.com/lmctl-ai/lmmolecule/main/examples/butane_torsion.lms` | united-atom butane chain with harmonic bonds/angles, 3-fold torsion, 1-4 LJ |
| [morse_dimer.lms](examples/morse_dimer.lms)<br>`lmctl autorun https://raw.githubusercontent.com/lmctl-ai/lmmolecule/main/examples/morse_dimer.lms` | Lepton custom bond Morse potential under NVE Verlet with energy conservation check |
| [combo/morse_plot.lms](examples/combo/morse_plot.lms)<br>`lmctl autorun https://raw.githubusercontent.com/lmctl-ai/lmmolecule/main/examples/combo/morse_plot.lms` | `lmmolecule` simulates, `lmplot` draws energy conservation plot |

## Build

C++17 compiler, CMake 3.24, git; nothing else.

```sh
scripts/sync-deps.sh            # OpenMM 8.2.0 at the commit in deps.lock
cmake -S . -B build && cmake --build build
./test.sh                       # needs lmscript on PATH
```

Linux builds are fully static; macOS links only system libraries; Windows links the MinGW runtime
statically. `lmmolecule --notices` prints the license texts of everything inside.

## License

MIT ([LICENSE](LICENSE)). OpenMM Core/Reference/CPU/Lepton are MIT/BSD/public-domain, PocketFFT is
BSD-3-Clause, SFMT is BSD-3-Clause, L-BFGS is MIT; see [THIRD_PARTY_NOTICES](THIRD_PARTY_NOTICES).
