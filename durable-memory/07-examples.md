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
