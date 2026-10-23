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
