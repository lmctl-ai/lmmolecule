"""A worker-thread budget failure must retain its builder slot and one envelope."""
import json, os, sys
from pathlib import Path
from invocation import run_sources

work = Path(sys.argv[1])
schema = str(Path('schema/lmmolecule.dsl.json').resolve())
first, last = work / 'worker-first.txt', work / 'worker-last.txt'
sources = []
for name, output in [('first', first), ('last', last)]:
    source = work / ('worker-' + name + '.lms')
    source.write_text('p = _lmcode_ schema=' + json.dumps(schema) + ';\n'
                      'file_write(' + json.dumps(str(output)) + ', "done");\nend _lmcode_\n')
    sources.append(source)
os.environ['LMMOLECULE_MAX_MEMORY_MB'] = '256'
result = run_sources([sources[0], work / 'cpunbl.lms', sources[1]])
body = json.loads(result.stdout)
assert result.returncode == 3 and body['ok'] is False, result
assert body['phase'] == 'run' and body['part'] == 1, result
assert 'memory budget exceeded' in result.stdout, result
assert 'LMMOLECULE_MAX_MEMORY_MB' in result.stderr, result
assert first.exists() and not last.exists(), result
print('PASS worker memory failure: builder part 1, exit 3, terminal frame, no later effects')
