"""Test-only run_lmcode drivers; capture private inputs and preserve host exit status."""
import json, os, re, shutil, subprocess, sys, tempfile
from pathlib import Path


def compiler_cmd():
    compiler = os.environ.get('LMSCRIPT_BIN', 'lmscript')
    if sys.platform == 'win32' and Path(compiler + '.cmd').exists():
        return ['cmd.exe', '/c', compiler + '.cmd']
    return [compiler]


def run_sources(sources, *, dry=False, capture=None, example=False, capture_only=False, check=False, timeout=600):
    host = str(Path(os.environ.get('LMMOLECULE_HOST', 'build/lmmolecule')).resolve())
    with tempfile.TemporaryDirectory(prefix='lmmolecule-invocation-') as directory:
        directory = Path(directory)
        status = directory / 'status'
        command = [sys.executable, str(Path(__file__).resolve()), '--capture-host' if capture_only else '--host', host,
                   str(status), str(Path(capture).resolve()) if capture else '', '--lmcode']
        if example:
            text = Path(sources[0]).read_text()
            # Only wrap this host's calls; optional lmplot calls keep their own binding.
            for name in re.findall(r'(\w+)\s*=\s*_lmcode_\s+cli="lmmolecule"', text):
                text = text.replace('run_lmcode(' + name + ')',
                    'run_lmcode(' + name + ', {command:' + json.dumps(command + ['$DSLFILE']) + '})')
        else:
            text = 'b = lmcode_build(' + json.dumps(command) + ');\n' if len(sources) > 1 else ''
            for i, source in enumerate(sources):
                chunk = Path(source).read_text().split('end _lmcode_', 1)[0] + 'end _lmcode_\n'
                chunk = re.sub(r'\bp\s*=\s*_lmcode_', f'p{i} = _lmcode_', chunk, count=1)
                text += chunk
                if len(sources) > 1:
                    text += f'b.add(p{i}, ["$DSLFILE"]);\n'
            text += ('r = run_lmcode(b);\n' if len(sources) > 1 else
                     'r = run_lmcode(p0, {command:' + json.dumps(command + ['$DSLFILE']) + '});\n')
            text += 'println(json_stringify(r));\n'
        driver = directory / 'driver.lms'
        driver.write_text(text)
        result = subprocess.run(compiler_cmd() + (['--check'] if check else ['--dry-run'] if dry else []) +
                                ['--base-dir', str(Path.cwd()), str(driver)],
                                capture_output=True, text=True, timeout=timeout)
        # Never mask a compiler/driver failure with a successful host status.
        if status.exists() and (result.returncode == 0 or int(status.read_text()) != 0):
            result.returncode = int(status.read_text())
        elif not check and not dry and not status.exists() and result.returncode == 0:
            result.returncode = 125
            result.stderr += "test driver did not execute the selected host\n"
        return result


if __name__ == '__main__':
    if sys.argv[1] in ('--host', '--capture-host'):
        host, status, capture, *args = sys.argv[2:]
        if capture:
            target = Path(capture); target.mkdir(parents=True, exist_ok=True)
            for i, source in enumerate(args[1:]):
                shutil.copyfile(source, target / f'{i}.lmcode')
        if sys.argv[1] == '--capture-host':
            assert capture and len(args) == 2
            # Bytes with bare LF: text-mode print would emit CRLF on Windows and the frame would not be terminal.
            sys.stdout.buffer.write(b'LMCODE_RETURN_BEGIN\n{"ok":true,"result":null}\nLMCODE_RETURN_END\n'); sys.stdout.buffer.flush()
            Path(status).write_text('0')
            sys.exit(0)
        result = subprocess.run([host, *args])
        Path(status).write_text(str(result.returncode))
        sys.exit(result.returncode)
    import argparse
    parser = argparse.ArgumentParser()
    parser.add_argument('--check', action='store_true')
    parser.add_argument('--example', action='store_true')
    parser.add_argument('--dry-run', action='store_true')
    parser.add_argument('--capture')
    parser.add_argument('--capture-only', action='store_true')
    parser.add_argument('sources', nargs='+')
    args = parser.parse_args()
    result = run_sources(args.sources, dry=args.dry_run, capture=args.capture, example=args.example, capture_only=args.capture_only, check=args.check,
                         timeout=3600 if args.example else 600)
    print(result.stdout, end=''); print(result.stderr, end='', file=sys.stderr)
    sys.exit(result.returncode)
