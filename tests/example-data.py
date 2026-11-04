"""Check molecular prefixes independently of unavailable rendering hosts."""
import re, subprocess, tempfile
from pathlib import Path
from invocation import compiler_cmd

root = Path.cwd()
sources = sorted((root / 'examples/visual').glob('*.lms')) + sorted((root / 'examples/stories').glob('*.lms'))
assert len(sources) == 20, len(sources)
checked = ran = 0
with tempfile.TemporaryDirectory(prefix='example-data-', dir=root / 'build') as scratch:
    scratch = Path(scratch)
    for source in sources:
        text = source.read_text()
        assert 'r.stdout' not in text, source
        renderer = re.search(r'^\w+\s*=\s*_lmcode_ cli="(?:lmplot|lmcad|lmdraw)"', text, re.M)
        assert renderer, source
        prefix = text[:renderer.start()]
        prefix = prefix.replace(' install="github:lmctl-ai/lmmolecule@latest"', '')
        program = scratch / source.name
        program.write_text(prefix)
        result = subprocess.run(compiler_cmd() + ['--check', str(program)], cwd=scratch,
                                capture_output=True, text=True, timeout=60)
        assert result.returncode == 0, (source.name, result.stdout, result.stderr)
        checked += 1
        # Full story simulations remain in the optional visual suite. Exercise
        # every small visual data producer offline as a required regression.
        if source.parent.name != 'visual':
            continue
        result = subprocess.run(compiler_cmd() + [str(program)], cwd=scratch,
                                capture_output=True, text=True, timeout=300)
        assert result.returncode == 0, (source.name, result.stdout, result.stderr)
        files = re.findall(r'output = read_file\("([^"]+)"\);', prefix)
        assert files and all((scratch / f).stat().st_size > 0 for f in files), source
        ran += 1
assert checked == 20 and ran == 8, (checked, ran)
print('PASS 20 molecular story/visual prefixes checked; 8 offline visual file handoffs executed')
