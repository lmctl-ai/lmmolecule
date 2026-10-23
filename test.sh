#!/usr/bin/env bash
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$HERE"
BUILD_DIR="${BUILD_DIR:-build}"
HOST="$BUILD_DIR/lmmolecule"
UNAME_S="$(uname -s)"
case "$UNAME_S" in MINGW*|MSYS*|CYGWIN*) HOST="$HOST.exe";; esac
WORK="$(mktemp -d "${TMPDIR:-/tmp}/lmmolecule-test-XXXXXX")"
trap 'rm -rf "$WORK"' EXIT
ROOT="$HERE"
# Native Windows tools (node, the host) cannot address git-bash /tmp or /d/a paths.
case "$UNAME_S" in MINGW*|MSYS*|CYGWIN*) WORK="$(cd "$WORK" && pwd -W | tr '\\' '/')"; ROOT="$(pwd -W | tr '\\' '/')";; esac
pass=0
ok(){ pass=$((pass+1)); echo "ok $pass - $1"; }
fail(){ echo "FAIL: $*" >&2; exit 1; }
sha(){ if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | cut -c1-64; else shasum -a 256 "$1" | cut -c1-64; fi; }

# lmscript resolves cli="lmmolecule" on PATH first, so programs bind to the binary under test.
export PATH="$(cd "$BUILD_DIR" 2>/dev/null && pwd || echo "$HERE/$BUILD_DIR"):$PATH"
LMSCRIPT="$(scripts/select-compiler.sh)"
scripts/sync-sdk.sh --verify >/dev/null || fail "SDK header pin mismatch"
ok "SDK header matches sdk.lock"
scripts/sync-deps.sh --verify >/dev/null || fail "third_party pin mismatch"
ok "third_party matches deps.lock"

GEN=""
command -v ninja >/dev/null 2>&1 && GEN="-G Ninja"
# shellcheck disable=SC2086
cmake -S . -B "$BUILD_DIR" $GEN -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$BUILD_DIR" >"$WORK/build.log" 2>&1 || { tail -40 "$WORK/build.log" >&2; fail "build"; }
ok "build"

want_version="lmmolecule $(sed -n 's/^project(lmmolecule VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)"
[ "$("$HOST" --version)" = "$want_version" ] || fail "--version is not exactly '$want_version'"
ok "--version prints exactly '$want_version'"
python3 scripts/gen-schema.py >/dev/null && git diff --quiet -- schema/lmmolecule.dsl.json 2>/dev/null || [ ! -e .git ] || fail "schema/lmmolecule.dsl.json is stale: run scripts/gen-schema.py"
ok "schema matches the registrations in cpp/main.cpp"
python3 scripts/check-visuals.py || fail "visual assets"
if { command -v lmplot >/dev/null 2>&1 && command -v lmcad >/dev/null 2>&1; } || [ "${LMMOLECULE_TEST_NETWORK:-}" = 1 ]; then
  examples/render-all.sh || fail "visual examples"
else
  ok "visual examples skipped (lmplot/lmcad unavailable, LMMOLECULE_TEST_NETWORK unset)"
fi
"$HOST" --dsl-schema --json >"$WORK/schema.json" || fail "embedded schema print"
cmp -s "$WORK/schema.json" schema/lmmolecule.dsl.json || fail "embedded schema differs from schema/lmmolecule.dsl.json"
ok "--dsl-schema --json prints byte-identical embedded schema bytes"
diff -u <("$LMSCRIPT" --dsl-signatures schema/lmmolecule.dsl.json) <("$HOST" --dsl-signatures) || fail "host/compiler signatures differ"
ok "host registry matches compiler signatures"
"$HOST" --notices | grep -q "OpenMM" || fail "--notices missing OpenMM text"
"$HOST" --notices | grep -q "PocketFFT" || fail "--notices missing PocketFFT text"
ok "--notices prints the third-party license texts"

case "$UNAME_S" in
  Linux) if ldd "$HOST" >/dev/null 2>&1; then fail "executable is dynamically linked"; fi; ok "executable is fully static (no dynamic loader)";;
  Darwin) bad="$(otool -L "$HOST" | tail -n +2 | grep -v -E '/usr/lib/(libSystem|libc\+\+|libresolv)' || true)"
          [ -z "$bad" ] || fail "non-system dylib dependencies: $bad"; ok "only system libraries linked";;
  MINGW*|MSYS*|CYGWIN*)
    if command -v objdump >/dev/null 2>&1; then
      bad="$(objdump -p "$HOST" | grep 'DLL Name' | grep -i -E 'libstdc|libgcc|winpthread|libgomp' || true)"
      [ -z "$bad" ] || fail "MinGW runtime DLLs are imported: $bad"
      ok "no MinGW runtime DLL imports"
    else ok "static-link check skipped (no objdump)"; fi;;
  *) ok "static-link check skipped on $UNAME_S";;
esac

# Programs: compile every test and example, run them (they assert their own numbers).
mkdir -p build/examples
n=0
for src in tests/*.lms examples/*.lms; do
  name="$(basename "$src" .lms)"
  "$LMSCRIPT" "$src" "build/$name.dslb" >"$WORK/$name.log" 2>&1 || { cat "$WORK/$name.log" >&2; fail "$name compile"; }
  n=$((n+1))
done
ok "$n programs compile as bytecode v5"
for src in tests/*.lms examples/*.lms; do
  name="$(basename "$src" .lms)"
  "$HOST" --dsl "build/$name.dslb" >"$WORK/$name.out" 2>"$WORK/$name.err" || { cat "$WORK/$name.out" "$WORK/$name.err" >&2; fail "$name run"; }
  grep -q '"ok": true' "$WORK/$name.out" || { cat "$WORK/$name.out" >&2; fail "$name did not report ok"; }
done
ok "$n programs run and pass their own assertions"
# Sim.rdf of a perfect 3x3x3 FCC lattice (written by tests/water_constraints.lms): the first shell holds 12 neighbours.
shell12="$(awk '!/^#/ && $1 < 0.45 { r1 = $1 - 0.005; r2 = $1 + 0.005; s += $2 * 108 / (1.62^3) * 4/3 * 3.141592653589793 * (r2^3 - r1^3) } END { printf "%.4f", s }' build/fcc_rdf.csv)"
[ "$shell12" = "12.0000" ] || fail "fcc g(r) first shell integrates to $shell12 neighbours, expected 12"
ok "Sim.rdf counts exactly 12 first neighbours in an FCC lattice"
# Sim.rdf_pair on the 4x4x4 rock-salt lattice (V = 2.256^3 nm^3, 256 ions of each kind): 6 Cl around Na, 12 Na around Na.
nacl6="$(awk '!/^#/ && $1 < 0.33 { r1 = $1 - 0.005; r2 = $1 + 0.005; s += $2 * 256 / (2.256^3) * 4/3 * 3.141592653589793 * (r2^3 - r1^3) } END { printf "%.4f", s }' build/nacl_rdf_nacl.csv)"
nana12="$(awk '!/^#/ && $1 < 0.45 { r1 = $1 - 0.005; r2 = $1 + 0.005; s += $2 * 256 / (2.256^3) * 4/3 * 3.141592653589793 * (r2^3 - r1^3) } END { printf "%.4f", s }' build/nacl_rdf_nana.csv)"
[ "$nacl6" = "6.0000" ] || fail "rock-salt Na-Cl first shell integrates to $nacl6, expected 6"
[ "$nana12" = "12.0000" ] || fail "rock-salt Na-Na first shell integrates to $nana12, expected 12"
ok "Sim.rdf_pair counts 6 Na-Cl and 12 Na-Na first neighbours in rock salt"

# Dry-run: validate bytecode and schema without running.
"$HOST" --dry-run --dsl build/harmonic_bond.dslb >"$WORK/dry.out" || fail "--dry-run failed"
grep -q '"ok": true' "$WORK/dry.out" || fail "--dry-run did not report ok"
ok "--dry-run validates bytecode without executing"

# Determinism: the CSV and XYZ files the examples write are identical on rerun.
for f in build/examples/*.csv build/examples/*.xyz; do echo "$(sha "$f") $f"; done >"$WORK/before.txt"
for src in examples/*.lms; do
  name="$(basename "$src" .lms)"
  "$HOST" --dsl "build/$name.dslb" >/dev/null 2>&1 || fail "$name rerun"
done
for f in build/examples/*.csv build/examples/*.xyz; do echo "$(sha "$f") $f"; done >"$WORK/after.txt"
cmp -s "$WORK/before.txt" "$WORK/after.txt" || fail "result files differ on rerun"
ok "all $(wc -l <"$WORK/after.txt" | tr -d ' ') result files are byte-identical on rerun"

# Error paths: each snippet must fail the run with exit 3 and a message naming the problem.
expect_error() { # name, snippet, expected message fragment
  cat >"$WORK/$1.lms" <<LMS
p = _dsl_ cli="lmmolecule";
$2
end _dsl_
savedsl(p, "$WORK/$1.dslb");
LMS
  "$LMSCRIPT" "$WORK/$1.lms" >/dev/null 2>"$WORK/$1.cerr" || { cat "$WORK/$1.cerr" >&2; fail "$1 compile"; }
  set +e; out="$("$HOST" --dsl "$WORK/$1.dslb" 2>/dev/null)"; code=$?; set -e
  [ "$code" = 3 ] || fail "$1: exit code $code, expected 3 ($out)"
  echo "$out" | grep -q "$3" || fail "$1: message lacks '$3': $out"
}
expect_error badplat 'md.platform("cuda");' "unknown platform"
expect_error badexpr 'let x = md.eval("2 +* 3");' "expression"
expect_error unknown 'let x = md.eval("foo + 1");' "unknown name"
expect_error nopart 'let s = md.system().sim("verlet", 0.001, 0.0, 0.0, 1);' "no particles"
expect_error badidx 'let s = md.system().add_particle(1.0, 0.0, 0.0, 0.0).pos(5, 0.0, 0.0, 0.0);' "outside 0..0"
expect_error badcut 'let s = md.system().box(1.0, 1.0, 1.0).add_particle(1.0, 0.0, 0.0, 0.0).coulomb("pme", 0.6).sim("verlet", 0.001, 0.0, 0.0, 1);' "less than half"
expect_error badlepton 'let s = md.system().add_particle(1.0, 0.0, 0.0, 0.0).custom_external("0.5*k*(x^2+w^2)", "k=1.0");' "unknown name 'w'"
expect_error badinteg 'let s = md.system().add_particle(1.0, 0.0, 0.0, 0.0).sim("rk4", 0.001, 0.0, 0.0, 1);' "unknown integrator"
expect_error assertfail 'md.assert_close(1.0, 2.0, 0.1);' "assert_close failed"
expect_error zerodt 'let s = md.system().add_particle(1.0, 0.0, 0.0, 0.0).sim("verlet", 0.0, 0.0, 0.0, 1);' "dt must be positive"
expect_error negmass 'let s = md.system().add_particle(-1.0, 0.0, 0.0, 0.0);' "mass must be non-negative"
expect_error diverge 'let s = md.system().add_particle(1.0, 0.0, 0.0, 0.0).add_particle(1.0, 0.3, 0.0, 0.0).bond(0, 1, 0.1, 1000000.0).sim("verlet", 0.1, 0.0, 0.0, 1).step(1000);' "simulation diverged"
expect_error nanenergy 'let s = md.system().add_particle(1.0, 0.0, 0.0, 0.0).add_particle(1.0, 0.5, 0.0, 0.0).custom_bond(0, 1, "log(r-1)").sim("verlet", 0.001, 0.0, 0.0, 1); let e = s.pe();' "energy is not finite"
expect_error nanconst 'let x = md.eval("sqrt(-1)");' "constant sub-expression is not finite"
expect_error reserved 'let s = md.system().add_particle(1.0, 0.0, 0.0, 0.0).custom_external("k*x^2", "x=1.0");' "is reserved"
expect_error ommclash 'let s = md.system().add_particle(1.0, 0.0, 0.0, 0.0).custom_external("k*x^2", "k=1.0").custom_external("k*y^2", "k=2.0").sim("verlet", 0.001, 0.0, 0.0, 1);' "different default values"
expect_error cutclash 'let s = md.system().fcc(2, 0.6, 40.0).lj(1.0, 0.34, 0.5).coulomb("pme", 0.55).sim("verlet", 0.001, 0.0, 0.0, 1);' "share one cutoff"
expect_error pmegrid 'let s = md.system().add_particle(1.0, 0.0, 0.0, 0.0).charge(0, 1.0).box(200.0, 200.0, 200.0).coulomb("pme", 0.5).sim("verlet", 0.001, 0.0, 0.0, 1);' "PME grid 8772x8772x8772 is too large"
expect_error barotemp 'let s = md.system().fcc(2, 0.6, 40.0).lj(1.0, 0.34, 0.5).barostat(1.0, 0.0, 25, 1);' "temperature must be positive"
expect_error crowded 'let s = md.system().fcc(10, 0.1, 1.0).gas(1, 1.0, 1.0, 1);' "box too crowded"
expect_error selfcons 'let s = md.system().add_particle(1.0, 0.0, 0.0, 0.0).constrain(0, 0, 0.1);' "to itself"
expect_error fixedcons 'let s = md.system().add_particle(0.0, 0.0, 0.0, 0.0).add_particle(1.0, 0.1, 0.0, 0.0).constrain(0, 1, 0.1).sim("verlet", 0.001, 0.0, 0.0, 1);' "zero-mass"
expect_error badwater 'let s = md.system().water("tip4p", 0.0, 0.0, 0.0);' "unknown water model"
expect_error baddiel 'let s = md.system().dielectric(0.5);' "must be >= 1"
expect_error rdfnobox 'let s = md.system().add_particle(1.0, 0.0, 0.0, 0.0).add_particle(1.0, 0.3, 0.0, 0.0).sim("verlet", 0.001, 0.0, 0.0, 1); s.rdf("'"$WORK"'/x.csv", 0.1, 10, 1, 0);' "requires a periodic box"
expect_error msdnomark 'let s = md.system().add_particle(1.0, 0.0, 0.0, 0.0).sim("verlet", 0.001, 0.0, 0.0, 1); let m = s.msd();' "Sim.mark()"
expect_error andersen0 'let s = md.system().add_particle(1.0, 0.0, 0.0, 0.0).andersen(0.0, 1.0, 1);' "temperature must be positive"
expect_error threads0 'md.threads(0);' "1..256"
expect_error nopdb 'let s = md.system().load_pdb("'"$WORK"'/missing.pdb");' "cannot open"
expect_error rdfsym 'let s = md.system().fcc(2, 0.6, 40.0).sim("verlet", 0.001, 0.0, 0.0, 1); s.rdf_pair("'"$WORK"'/x.csv", "Ar", "Xe", 0.2, 10, 1, 0);' "no particles with symbol 'Xe'"
ok "errors stop the program with exit 3 and a clear message (30 cases)"

# Memory budget: a runaway allocation loop stops with a run error (exit 3, message on stderr and in
# the JSON line) instead of exhausting the machine. A small budget everywhere; on Linux also the
# default 4096 MiB under a 6 GiB address-space limit (ulimit only here, never in the host).
budget_prog() { # name, snippet
  cat >"$WORK/$1.lms" <<LMS
p = _dsl_ cli="lmmolecule";
$2
end _dsl_
savedsl(p, "$WORK/$1.dslb");
LMS
  "$LMSCRIPT" "$WORK/$1.lms" >/dev/null || fail "$1.lms compile"
}
expect_budget() { # name, MiB, what
  set +e; out="$(LMMOLECULE_MAX_MEMORY_MB=$2 "$HOST" --dsl "$WORK/$1.dslb" 2>"$WORK/$1.err")"; code=$?; set -e
  [ "$code" = 3 ] || fail "$3 under LMMOLECULE_MAX_MEMORY_MB=$2: exit $code, expected 3 ($out)"
  echo "$out" | grep -q "memory budget exceeded" || fail "$3: JSON lacks the budget message: $out"
  grep -q "LMMOLECULE_MAX_MEMORY_MB" "$WORK/$1.err" || fail "$3: no budget message on stderr"
}
# 108000 particles (13 MiB) per System, kept in the arena until exit.
budget_prog runaway 'let i = 0;
while (i < 1000000) {
  let s = md.system().fcc(30, 0.5, 1.0);
  i = i + 1;
}'
expect_budget runaway 64 "runaway loop"
# CPU platform: a dense cluster in a large box makes the neighbour list (built on OpenMM's worker
# thread) far larger than the particle data; the failure there must still be a clean run error.
budget_prog cpunbl 'md.platform("cpu");
let s = md.system().fcc(20, 0.05, 40.0).box(20.0, 20.0, 20.0).lj(0.0, 0.34, 1.5).sim("verlet", 0.001, 0.0, 0.0, 1);
let e = s.pe();'
expect_budget cpunbl 256 "CPU neighbour list"
# PME grid malloc'd by OpenMM (439^3 complex, 1291 MiB) is counted when the Sim is created.
budget_prog pmebig 'let s = md.system().add_particle(1.0, 0.0, 0.0, 0.0).charge(0, 1.0).box(10.0, 10.0, 10.0).coulomb("pme", 0.5).sim("verlet", 0.001, 0.0, 0.0, 1);'
expect_budget pmebig 1024 "PME grid"
set +e; LMMOLECULE_MAX_MEMORY_MB=-5 "$HOST" --dsl "$WORK/runaway.dslb" >/dev/null 2>"$WORK/badenv.err"; code=$?; set -e
[ "$code" = 2 ] && grep -q "positive whole number" "$WORK/badenv.err" || fail "bad LMMOLECULE_MAX_MEMORY_MB: exit $code"
if [ "$UNAME_S" = Linux ]; then
  set +e; out="$(ulimit -v 6291456; "$HOST" --dsl "$WORK/runaway.dslb" 2>/dev/null)"; code=$?; set -e
  [ "$code" = 3 ] && echo "$out" | grep -q "limit 4096 MiB" || fail "runaway loop with the default budget under ulimit -v 6 GiB: exit $code ($out)"
  ok "memory budget: runaway, CPU neighbour list and PME grid stop at small budgets; runaway stops at the default 4096 MiB (under ulimit -v 6 GiB)"
else
  ok "memory budget: runaway, CPU neighbour list and PME grid stop at small budgets (default-budget run is Linux-only)"
fi

# Combined pipeline (lmmolecule computes, lmplot draws). Needs lmplot on PATH, or network for install=.
if command -v lmplot >/dev/null 2>&1 || [ "${LMMOLECULE_TEST_NETWORK:-}" = 1 ]; then
  rm -f build/examples/morse_oscillator.png
  "$LMSCRIPT" examples/combo/morse_plot.lms >"$WORK/combo.log" 2>&1 || { cat "$WORK/combo.log" >&2; fail "combo example"; }
  [ -s build/examples/morse_oscillator.png ] || fail "combo example wrote no PNG"
  ok "lmmolecule + lmplot pipeline draws the Morse oscillator"
else
  ok "combo example skipped (no lmplot, LMMOLECULE_TEST_NETWORK unset)"
fi

set +e; "$HOST" --dsl "$WORK/does-not-exist.dslb" >/dev/null 2>&1; code=$?; set -e
[ "$code" = 3 ] || fail "missing bytecode exit code $code, expected 3"
set +e; "$HOST" >/dev/null 2>&1; code=$?; set -e
[ "$code" = 2 ] || fail "usage exit code $code, expected 2"
ok "usage and load errors use exit codes 2 and 3"

echo "all $pass checks passed"
