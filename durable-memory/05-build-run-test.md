# Build, run, test

Measured 2026-10-23, x86_64 Linux, GCC 11.4, Ninja.

- Clean Release build with OpenMM 8.2.0 (`openmmapi`, `olla`, `reference`, `cpu`, `lepton`,
  `pocketfft`, `sfmt`, `lbfgs`, `serialization`): about 45 s. Binary 5.1 MB (stripped, `-static`),
  `ldd`: "not a dynamic executable".
- `./test.sh` (16 checks): sdk pin; deps pin; build; `--version` exactly `lmmolecule 0.1.0`;
  schema equals the registrations in `cpp/main.cpp`; embedded schema byte-identical; host registry
  equals compiler signatures; `--notices` includes OpenMM and PocketFFT texts; static linking;
  every program under `tests/` (5 files) and `examples/` (4 files) compiles and runs and passes
  its own `assert_close` physics checks; `--dry-run` check; all 4 result CSV/XYZ files
  byte-identical on rerun; 9 error snippets exit 3 with a message naming the problem; the
  `lmmolecule` + `lmplot` combo pipeline (`examples/combo/morse_plot.lms`); usage/load exit codes
  2 and 3.
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
