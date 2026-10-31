# Build, run, test

Measured 2026-10-23, x86_64 Linux, GCC 11.4, Ninja.

- Clean Release build with OpenMM 8.2.0 (`openmmapi`, `olla`, `reference`, `cpu`, `lepton`,
  `pocketfft`, `sfmt`, `lbfgs`, `serialization`): about 45 s. Binary 5.1 MB (stripped, `-static`),
  `ldd`: "not a dynamic executable".
- `./test.sh` (19 checks, about 90 s with the build cached): sdk pin; deps pin; build; `--version`
  exactly `lmmolecule 0.2.0`; schema equals the registrations in `cpp/main.cpp` (the check diffs
  the working tree against the index, so stage `schema/lmmolecule.dsl.json` after regenerating);
  visual assets; embedded schema byte-identical; host registry equals compiler signatures;
  `--notices` includes OpenMM and PocketFFT texts; static linking; every program under `tests/`
  (8 files) and `examples/` (4 files) compiles and runs and passes its own `assert_close` physics
  checks; `Sim.rdf` of a perfect FCC lattice integrates to exactly 12 first neighbours;
  `--dry-run` check; all 7 result CSV/XYZ files byte-identical on rerun; 30 error snippets exit 3
  with a message naming the problem; the `lmmolecule` + `lmplot` combo pipeline
  (`examples/combo/morse_plot.lms`); usage/load exit codes 2 and 3.
- `lmscript` resolves every `install=` binding through the GitHub API even when the host is on
  `PATH`; without `GITHUB_TOKEN` the unauthenticated limit (60/h) turns into `GitHub API HTTP 403`
  compile errors after a dozen runs. Export `GITHUB_TOKEN=$(gh auth token)` locally.
- Physics oracles verified by `tests/*.lms`:
  1. `harmonic_bond.lms`: exact harmonic period $T = 2\pi\sqrt{\mu/k} = 1.0\text{ ps}$, quarter/half/full
     period turning points, analytical force, and NVE Verlet energy conservation ($|\Delta E/E_0| < 10^{-5}$
     over $10^4$ steps).
  2. `lj_dimer.lms`: exact Lennard-Jones 12-6 potential and force at $r = 0.40\text{ nm}$, L-BFGS
     minimization to $r_{\min} = 2^{1/6}\sigma$ and $V_{\min} = -\varepsilon$, and 1-2 exclusion.
  3. `coulomb_madelung.lms`: vacuum two-charge Coulomb law and 3D periodic $4\times 4\times 4$ NaCl
     rock-salt crystal PME lattice energy reproducing the textbook Madelung constant
     $M = 1.7475645946...$ to $< 2\times 10^{-5}$ on both Reference and SIMD CPU platforms.
  4. `valence_custom.lms`: harmonic angle energy and geometry, periodic torsion energy, Lepton
     custom bond Morse minimization, custom nonbonded + custom external harmonic trap, and XYZ
     save/load round-trip.
  5. `thermostat_ensembles.lms`: deterministic seed reproducibility, canonical equipartition
     $\langle \text{KE}\rangle = \langle \text{PE}\rangle = \frac{3}{2} N k_B T$ for 256 particles
     in a 3D harmonic well under BAOAB Langevin dynamics at $300\text{ K}$, and NPT Monte Carlo
     barostat.
  6. `water_constraints.lms`: TIP3P dimer $-6.50$ kcal/mol at $R_{OO} = 2.74$ Å (Jorgensen 1983)
     within 0.3 kJ/mol and 0.003 nm, constraint lengths to $10^{-5}$ nm under Langevin dynamics,
     `dof` = 162 for 27 rigid waters, PDB round trip, reaction-field closed forms for dielectric
     78.3 and 1.0, free-flight MSD $= v^2 t^2$, FCC $g(r)$ first shell = 12, rock-salt `rdf_pair` 6 Na-Cl and 12 Na-Na neighbours.
  7. `thermostats_threads.lms`: Andersen thermostat equipartition within 8 %, two CPU threads
     keep the harmonic NVE drift below $10^{-8}$.

## Native host flag contract

The host accepts only the lmcode option family, with no retired aliases. The full gate checks exact schema/signature output, new loader routing and rejection before file effects in scratch HOME. SDK20325f7 is synchronized through the owner script for actual source-compiler v6 capabilities; package metadata alone is insufficient. Internal DSLB and .dslb names remain.
