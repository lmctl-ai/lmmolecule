# Examples

`examples/*.lms` each assert their own physical results; `examples/combo/morse_plot.lms` runs two
hosts in one script (`lmmolecule` for OpenMM NVE Verlet integration of an anharmonic Morse
oscillator, `lmplot` for plotting kinetic, potential, and total energy vs time).

- `examples/argon_fcc_melt.lms`: builds a $3\times 3\times 3$ FCC argon crystal (108 atoms),
  minimizes lattice energy with L-BFGS ($\sim -7.74\text{ kJ/mol}$ per atom), and equilibrates at
  $120\text{ K}$ with the BAOAB Langevin middle integrator while writing `argon_fcc.csv` and
  `argon_fcc.xyz`.
- `examples/nacl_madelung.lms`: builds a $4\times 4\times 4$ rock-salt NaCl supercell (512 ions)
  and computes the Madelung constant $M = 1.747567$ via PME on the CPU platform.
- `examples/butane_torsion.lms`: relaxes a united-atom butane chain from an eclipsed conformation
  into its trans minimum using harmonic bonds, angles, 3-fold periodic torsion, and 1-4 LJ with
  1-2/1-3 exclusions.
- `examples/morse_dimer.lms`: integrates a diatomic molecule with a Lepton custom bond Morse
  potential and checks NVE energy conservation.

## Reproducible visuals

README opens with six captioned visuals. `docs/gallery.md` lists eight new scientific stories in
`examples/visual/`: argon_structure, nacl_structure, butane_relaxation, lj_binding, water_bend,
isotope_oscillation, thermal_trap, morse_motion. The original four examples execute themselves and draw their own results; the existing combo also copies its plot to `docs/images/`.

Run `LMMOLECULE_TEST_NETWORK=1 examples/render-all.sh` from any directory, after building the
host locally or making a release available on PATH. Released sibling hosts auto-install through
lmscript. lmplot draws measurements and lmcad renders computed coordinate CSVs (no hand-placed
replacement results). `ffmpeg` assembles 48 computed Morse frames into a looping GIF; it is an
external executable and no new library is linked. Intermediate CSV/bytecode/frames stay under
ignored `build/visuals/`; only PNG/GIF assets under `docs/images/` are committed. Sphere sizes
are illustrative; all molecular positions are in nm. Seeded Reference-platform runs are repeatable
on one platform; no cross-platform byte stability is promised for fonts or stochastic integrators.

`test.sh` checks source/image references, companion coverage, signatures and size bounds, then
regenerates visuals when sibling hosts are available (otherwise prints an explicit skip).

All gallery captions include raw-main `lmctl autorun` commands. Examples are self-contained and
write into the current working directory, including when fetched into an empty directory.
