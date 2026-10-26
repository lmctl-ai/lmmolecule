# Examples

Three groups, all self-executing (`savedsl` then `system ${dsl_cli(p)} --dsl ...`), all writing
images under `docs/images/` of the current directory, all reproducible with
`lmctl https://raw.githubusercontent.com/lmctl-ai/lmmolecule/main/examples/<path>.lms`
from an empty directory. Every example asserts its physical result with `md.assert_close`.

- `examples/*.lms` (4): argon_fcc_melt, nacl_madelung (Madelung 1.747567 by PME), butane_torsion,
  morse_dimer. They keep the compile-only form (`lmscript file.lms out.dslb`) that `test.sh` uses.
- `examples/visual/*.lms` (8) and `examples/combo/morse_plot.lms`: the first picture set (lmcad
  renders of coordinate CSVs, lmplot curves, the Morse GIF).
- `examples/stories/*.lms` (12, added in 0.2.0): worked problems with a known answer.

## Stories and their oracles

| Example | Known answer | Hosts |
| --- | --- | --- |
| water_box | 216 TIP3P, PME 0.9 nm, 300 K, 4 threads: U/N about -40 kJ/mol (Jorgensen -41.1), O-O peak 2.75 A, O-H peak 1.85 A, dof = 1296 | lmplot 2x2 multiplot with an embedded lmcad PPM, lmcad render with the cell wireframe |
| water_dimer | minimized dimer -6.46 kcal/mol at 2.75 A (Jorgensen 1983: -6.50 at 2.74), O-H...O 178 deg; rigid O-O scan | lmdraw schematic from `${}`-interpolated coordinates, lmplot curve |
| water_forces | bent flexible water: strain = 0.5 k (theta - theta0)^2 exactly, forces sum to 1e-6 | lmdraw force diagram |
| argon_rdf_phases | FCC shells at a/sqrt(2) and a; liquid first peak 1.1 sigma; gas g = 1 | lmplot |
| diffusion_msd | D = 2.5e-5 cm^2/s (Rahman 1964: 2.43e-5), asserted in [0.8, 4]e-5; gnuplot `fit` + `plot.num` | lmplot |
| npt_density | 1.35 g/cm^3 at 1 bar, 94.4 K (measured 1.374); random-gas start, dispersion correction on | lmplot |
| maxwell_boltzmann | 6000 speeds vs analytic f(v); binning by gnuplot `smooth frequency` | lmplot |
| argon_melt | 24 Langevin stages 20-158 K; one lmcad process per frame, scene scaled by 0.2 for speed; ffmpeg GIF | lmcad, ffmpeg, lmplot still from PPM frames |
| timestep_convergence | Verlet error proportional to dt^2 up to 3 fs, unstable above 2/omega = 10.8 fs | lmplot |
| morse_period | T(E) = T0 / sqrt(1 - E/De) within 1 % for E/De = 0.1..0.9; harmonic stays T0 | lmplot |
| thermostat_comparison | Langevin 1/ps, 10/ps and Andersen 10/ps reach 120 K within 40 K; NVE stays cold | lmplot |
| phonon_dispersion | 16 ring modes on omega = 2 sqrt(K/m) sin(ka/2) within 3 % | lmplot |

## How the pictures are made

- Data flow host -> picture: the compute block prints CSV lines (`md.print`) or writes files
  (`Sim.report`, `Sim.rdf`, `Sim.save_pdb`); the outer script splits stdout at `{"ok"` and writes
  CSV; lmplot reads CSV, lmcad reads `t,x,y,z,r,g,b` keyframe CSV (times strictly increasing: use
  the row index), lmdraw has no file input so numbers reach it as `${name}` tokens interpolated
  into the `_dsl_` block (whole-number floats become int literals: keep coordinates non-integral).
- lmdraw writes only under `build/rendered/`; copy to `docs/images/` afterwards. lmplot's native
  terminal has no `{/Symbol}` font and cannot read PNG; use plain text and `binary array=(w,h)
  skip=15 format='%uchar' with rgbimage` on lmcad PPM output.
- lmcad tessellates with an absolute tolerance: a molecular frame in nm units renders about eight
  times faster when the scene is scaled by 0.2 (recorded in simulation/DRAW_NEEDS.md).
- OpenMM never wraps positions: renders wrap each molecule by its oxygen (`floor(x / L)`), and
  `Sim.msd` relies on the unwrapped coordinates.
- Timings (x86_64 laptop, build cached): all stories under 15 s except water_box (about 90 s with
  `md.threads(4)`) and argon_melt (about 30-50 s, lmcad frames). `examples/render-all.sh` runs all
  25 examples; CI runs it with `LMMOLECULE_TEST_NETWORK=1`.
- `lmctl autorun` installs the latest *release*; the stories need the 0.2.0 API, so they only work
  through autorun once a `v0.2.0` release exists (local `build/` on `PATH` takes priority).

## README layout

Six hero images before the first `##` heading (check-visuals requires 3-6), then `## Gallery by
theme` with five `###` sections covering all 29 images, the one-minute start, the API table, the
example list, limits, build, resource limits, license. `docs/gallery.md` mirrors the themes with
one picture per section.
