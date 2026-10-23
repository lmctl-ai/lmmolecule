# Resource limits (total memory budget)

Why: host objects (`System`, `Sim` with its OpenMM `Context`) live in arenas until exit, so a DSL loop
that builds systems or simulations grows without bound (a runaway `lmnum` once reached 16 GB RSS on a
24 GB machine). Every lm* host now has a total budget: default 4096 MiB, env
`LMMOLECULE_MAX_MEMORY_MB` (positive whole MiB; a bad value prints usage, exit 2). Over budget is a run
error: exit 3, one line on stderr naming the variable, the usual JSON error line on stdout.

How it is counted here
- `cpp/memory_budget.{hpp,cpp}` is the shared copy from lmnum (b932657), unchanged: replacement global
  `operator new/delete` (plain and aligned) count allocator block sizes. OpenMM defines no operator new
  of its own (checked with `nm` on libopenmm.a; the link would fail otherwise), and almost everything it
  keeps (System, forces, Context, Reference/CPU platform data, neighbour lists, State copies) is
  `std::vector`/`new`, so it is counted automatically.
- Memory OpenMM takes with `malloc` that the host accounts for explicitly:
  - PME (`ReferencePME`, used by both platforms; the CPU-PME plugin is not built) mallocs the complex
    grid plus B-spline tables at every force evaluation and never checks for NULL. `build_sim` computes
    the grid with `NonbondedForceImpl::calcPMEParameters` before the Context exists, rejects grids over
    2^31 cells, and `charge()`s `16*nx*ny*nz + 156*N` bytes for the Sim's life (released in `~SimObj`).
    Box 10 nm, cutoff 0.5 nm gives 439^3 (1291 MiB); box 200 nm gives 8772^3 (rejected).
  - L-BFGS (`Sim.minimize`) mallocs about 18 vectors of 3N doubles: `require()`d before minimizing.
- Bulk builders (`fcc`, `rocksalt`, `gas`, `load_xyz`) can be called repeatedly on one System: total
  particles capped at 10,000,000 (OpenMM indexes with int) and the vector growth is `require()`d first.
- CPU platform worker thread: OpenMM's `ThreadPool` runs tasks (force kernels, neighbour-list build) on a
  pthread even with Threads=1, and its thread body has no handler, so an `Exceeded` thrown there
  (typically while the neighbour list grows) would call `std::terminate`. `main` installs a terminate
  handler that recognizes `membudget::Exceeded`, prints the JSON run error and `_Exit(3)`; any other
  exception falls through to the previous handler (abort). That JSON line has no `dsl line` location.
  Main-thread failures go through `translate()` into a normal `dslvm::Error` with location.
- `main` catches `std::exception` around `Program::load` and `Program::run` as well.

Not covered
- Thread stacks of the CPU platform (one worker per CPU `Sim`, about 8 MiB virtual each, mostly not
  resident), static data, and the C/C++ runtime's own overhead.
- `quern` (CCMA constraint solver, malloc) is compiled but unreachable: the schema has no constraints.
- No JIT: asmjit / `LEPTON_USE_JIT` are not built, so there is no mmap'd executable memory.
- Report/XYZ trajectories stream to files; nothing is buffered beyond one State copy.
- Counted capacity can exceed resident memory (untouched pages), so RSS is usually a bit below the limit.

Measured (Linux x64, WSL2, Release)
- Runaway `md.system().fcc(30, 0.5, 1.0)` in a loop (13 MiB per System): 64 MiB budget -> exit 3 in
  0.19 s, peak RSS 66 MB; default 4096 MiB -> exit 3 in 9.2 s, peak RSS 4.19 GB (also passes under
  `ulimit -v 6291456`).
- CPU neighbour list (32000 atoms in a 1 nm cluster, 20 nm box, cutoff 1.5): 256 MiB -> exit 3 in 0.9 s
  from the worker thread, peak RSS 222 MB.
- Loop of CPU Sims (fcc(10), 4000 atoms, LJ cutoff 1.0): 512 MiB -> exit 3 in 11 s, peak RSS 532 MB.
- test.sh: runaway, CPU neighbour list and PME grid at small budgets on every platform, bad env value
  (exit 2), and on Linux the default budget under `ulimit -v 6291456`; whole test.sh about 100 s here.
