# Invocation envelope (0.2.2)

SDK lmcode-cpp origin/main 93bddf1; lmscript 0.2.460.
Public programs call run_lmcode and inspect r.ok. Metadata/arguments validate first,
all registered inputs load before execution, then select/run/complete runs each
part in registration order. The first failure stops the invocation. One SDK frame
reports exit 0 success, 2 CLI/metadata, 3 load/run, 4 application/output failure.
Dry mode validates and loads but executes no block; contradictory real metadata
and a standalone dry flag are rejected. Introspection remains metadata-independent.

md.print uses stderr. Numerical and plot-derived data handoffs use files, read only
after r.ok. Renderers must already be compatible on PATH; CI never installs them.
The former water_box manual child argv/timeout flow now uses run_lmcode (which has
no system-command default timeout). The two ffmpeg calls retain system exit codes.

OpenMM CPU worker budget exceptions reach a terminate handler. It releases the
failure reserve and reports the active builder part through InvocationResults,
flushes once, and exits. Concurrent failing workers wait for the sole writer.
A dedicated builder regression checks part 1, exit 3 and no later effects.
All original physics, neighbour-count, determinism, 30 error, memory, schema,
metadata and image assertions remain. Private input capture replaces exports.
Twenty molecular example prefixes are checked; eight small visual calculations
and their file handoffs execute offline. Full stories/renderers remain optional.
