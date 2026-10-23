# Molecular experiments

Every example executes itself and writes into the current working directory. `examples/render-all.sh` rebuilds this gallery.
The pipeline is OpenMM computation → CSV in `build/visuals/` → `lmplot` or `lmcad`.

## Minimized FCC argon

![Minimized FCC argon](images/argon_structure.png)

Coordinates in nm; illustrative sphere radii. [Source](../examples/visual/argon_structure.lms)<br>`lmctl autorun https://raw.githubusercontent.com/lmctl-ai/lmmolecule/main/examples/visual/argon_structure.lms`.

## Rock-salt lattice

![Rock-salt lattice](images/nacl_structure.png)

Blue Na+, green Cl-. Coordinates come from the charged lattice builder. [Source](../examples/visual/nacl_structure.lms)<br>`lmctl autorun https://raw.githubusercontent.com/lmctl-ai/lmmolecule/main/examples/visual/nacl_structure.lms`.

## Butane minimization

![Butane minimization](images/butane_relaxation.png)

Orange initial and teal relaxed united-atom chains; copies are translated apart for comparison. [Source](../examples/visual/butane_relaxation.lms)<br>`lmctl autorun https://raw.githubusercontent.com/lmctl-ai/lmmolecule/main/examples/visual/butane_relaxation.lms`.

## Lennard-Jones binding

![Lennard-Jones binding](images/lj_binding.png)

Actual pair energies across separation reveal the attractive well and repulsive wall. [Source](../examples/visual/lj_binding.lms)<br>`lmctl autorun https://raw.githubusercontent.com/lmctl-ai/lmmolecule/main/examples/visual/lj_binding.lms`.

## Water bending

![Water bending](images/water_bend.png)

A harmonic angle model at fixed O-H lengths, not a complete water force field. [Source](../examples/visual/water_bend.lms)<br>`lmctl autorun https://raw.githubusercontent.com/lmctl-ai/lmmolecule/main/examples/visual/water_bend.lms`.

## Mass and vibration

![Mass and vibration](images/isotope_oscillation.png)

Identical harmonic force constants with particle masses 1 and 4 amu. [Source](../examples/visual/isotope_oscillation.lms)<br>`lmctl autorun https://raw.githubusercontent.com/lmctl-ai/lmmolecule/main/examples/visual/isotope_oscillation.lms`.

## Langevin thermal sampling

![Langevin thermal sampling](images/thermal_trap.png)

Kinetic and potential energy fluctuations for one particle in a 300 K bath. [Source](../examples/visual/thermal_trap.lms)<br>`lmctl autorun https://raw.githubusercontent.com/lmctl-ai/lmmolecule/main/examples/visual/thermal_trap.lms`.

## Anharmonic bond motion

![Anharmonic bond motion](images/morse_motion.png)

A stretched Morse dimer under NVE Verlet; animation time is slowed for viewing. [Source](../examples/visual/morse_motion.lms)<br>`lmctl autorun https://raw.githubusercontent.com/lmctl-ai/lmmolecule/main/examples/visual/morse_motion.lms`.

![Morse bond trajectory animation](images/morse-motion.gif)

48 computed frames at 12 fps; `ffmpeg` assembles the GIF from lmcad PNG frames.

## Original examples

![argon fcc melt](images/argon_fcc_melt.png)

[Simulation source](../examples/argon_fcc_melt.lms)<br>`lmctl autorun https://raw.githubusercontent.com/lmctl-ai/lmmolecule/main/examples/argon_fcc_melt.lms`.

![nacl madelung](images/nacl_madelung.png)

[Simulation source](../examples/nacl_madelung.lms)<br>`lmctl autorun https://raw.githubusercontent.com/lmctl-ai/lmmolecule/main/examples/nacl_madelung.lms`.

![butane torsion](images/butane_torsion.png)

[Simulation source](../examples/butane_torsion.lms)<br>`lmctl autorun https://raw.githubusercontent.com/lmctl-ai/lmmolecule/main/examples/butane_torsion.lms`.

![morse dimer](images/morse_dimer.png)

[Simulation source](../examples/morse_dimer.lms)<br>`lmctl autorun https://raw.githubusercontent.com/lmctl-ai/lmmolecule/main/examples/morse_dimer.lms`.

![morse plot](images/morse_plot.png)

[Simulation source](../examples/combo/morse_plot.lms)<br>`lmctl autorun https://raw.githubusercontent.com/lmctl-ai/lmmolecule/main/examples/combo/morse_plot.lms`. 

