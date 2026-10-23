# Open items

Done in 0.2.0: distance constraints and rigid TIP3P / SPC/E water, PDB read/write, reaction-field
dielectric, Andersen thermostat, CPU thread count, `gas` overlap error, barostat T > 0 and box
checks, `Sim.rdf` / `Sim.mark` / `Sim.msd`.

- Force-field XML template matching (AMBER/CHARMM): `load_pdb` gives coordinates and masses only;
  bonds, angles, charges and LJ still come from the script.
- Custom angles (`CustomAngleForce`), custom torsions (`CustomTorsionForce`), and custom integrators
  (`CustomIntegrator`) are compiled in `libopenmm.a` and can be added to the schema.
- Implicit solvent (`GBSAOBCForce`), anisotropic barostats (`MonteCarloAnisotropicBarostat`),
  four-site water (TIP4P-Ew needs virtual sites).
- No `CMMotionRemover`: a thermostatted system can drift as a whole; `Sim.msd` includes that drift.
- Host objects live in arenas until the process exits; a `release()` method would help long loops
  that construct many separate `Sim` contexts.
- `Sim.rdf` is an O(N^2) pair loop (capped at 20000 particles); a cell list would lift the cap.
- `load_pdb` reads the first model only and ignores alternate locations and insertion codes.
