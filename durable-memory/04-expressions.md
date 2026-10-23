# Expression language (OpenMM Lepton)

`lmmolecule` uses OpenMM's embedded `Lepton` library (`Lepton::Parser::parse`) to parse, validate,
analytically differentiate, and evaluate mathematical expressions:

- `md.eval(expr)`: evaluates scalar expressions with built-in constants:
  - `pi` = $3.14159265358979323846$
  - `e` = $2.71828182845904523536$
  - `kB` = $0.00831446261815324\text{ kJ/(mol}\cdot\text{K)}$ (molar gas constant $R$)
  - `one_4pi_eps0` = $138.935457644382\text{ kJ}\cdot\text{nm}/(\text{mol}\cdot e^2)$
- `System.custom_external(expr, params)`: per-particle potential $V(x, y, z)$ in `kJ/mol` as a
  function of particle coordinates `x`, `y`, `z` (`nm`), `pi`, `e`, and semicolon- or
  comma-separated global parameters (`"k=50.0;x0=1.0"`).
- `System.custom_bond(i, j, expr)`: bond potential $V(r)$ in `kJ/mol` as a function of bond
  distance `r` (`nm`), `pi`, and `e` (e.g., Morse potential `"120*(1-exp(-12*(r-0.16)))^2"`).
- `System.custom_nonbonded(expr, params, cutoff)`: pairwise potential $V(r)$ in `kJ/mol` as a
  function of interparticle distance `r` (`nm`), `pi`, `e`, and named parameters in `params`.

Supported Lepton operators and functions: `+`, `-`, `*`, `/`, `^`, `sqrt`, `exp`, `log`, `sin`,
`cos`, `tan`, `asin`, `acos`, `atan`, `sinh`, `cosh`, `tanh`, `erf`, `erfc`, `min`, `max`, `abs`,
`floor`, `ceil`, `step`, `delta`, `select`. Unknown variable names are rejected immediately with
exit code 3 (`unknown name '...'`).
