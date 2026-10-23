# Open items

- Holonomic distance constraints (`System::addConstraint`, SHAKE / SETTLE) and rigid water models
  (TIP3P / SPC/E) are compiled in OpenMM's static library and can be exposed as additional `System`
  methods.
- Custom angles (`CustomAngleForce`), custom torsions (`CustomTorsionForce`), and custom integrators
  (`CustomIntegrator`) are compiled in `libopenmm.a` and can be added to the schema.
- PDB file reader/writer and force-field XML template matching (AMBER/CHARMM).
- Implicit solvent (`GBSAOBCForce`) and anisotropic barostats (`MonteCarloAnisotropicBarostat`).
- Radial distribution function $g(r)$ and mean-squared displacement $\text{MSD}(t)$ post-processing
  helpers returning CSV tables.
- Host objects live in arenas until the process exits; a `release()` method would help long loops
  that construct many separate `Sim` contexts.
- `coulomb("cutoff")` is reaction field with OpenMM's default dielectric 78.3; expose the dielectric or a
  plain shifted-cutoff option if users need it.
- `System.gas` places a particle anyway after 200 rejected trial positions (only reachable at very high
  densities); it could report an error instead.
- Thread count is fixed at 1 on the CPU platform (deterministic); a `threads` setting would help large
  systems at the cost of byte-identical reruns.
