#!/usr/bin/env python3
"""CLI regression against the real decoder (original synthetic XEX input)."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile

decoder, fixture, repo = map(lambda s: Path(s).resolve(), sys.argv[1:])
with tempfile.TemporaryDirectory(prefix='decode cli ', dir=repo / 'build') as tmp:
    directory = Path(tmp)
    # Quote and backslash are legal POSIX filename bytes, testing JSON escaping.
    source = directory / 'input "quoted".xex'
    source.write_bytes(fixture.read_bytes())
    out = directory / 'output "quoted".xex'
    source_arg = str(source)
    if sys.platform == 'cygwin':
        # Cygwin accepts a Windows input path (backslashes are separators,
        # not legal filename bytes on this host).
        source_arg = subprocess.check_output(['cygpath', '-w', str(source)], text=True).strip()
    result = subprocess.run([str(decoder), source_arg, str(out)], capture_output=True, text=True, timeout=20)
    assert result.returncode == 0, result.stderr
    report = json.loads(result.stdout)
    assert report['in'] == source_arg and report['out'] == str(out)
    assert report['bytes'] == out.stat().st_size
    assert out.read_bytes() == source.read_bytes()
    assert 'decoded image' in result.stderr
    # Resolving only the parent used to allow this symlink into a worktree.
    with tempfile.TemporaryDirectory(prefix='decoder worktree ', dir=directory) as tree:
        tree = Path(tree)
        (tree / '.git').mkdir()
        target = tree / 'tracked-data.xex'
        target.write_bytes(b'original sentinel')
        link = directory / 'escape.xex'
        link.symlink_to(target)
        result = subprocess.run([str(decoder), str(source), str(link)], capture_output=True, text=True, timeout=20)
        assert result.returncode == 2, result.stderr
        assert not result.stdout and target.read_bytes() == b'original sentinel'
print('decode_cli/json_stream_paths_and_output_symlink PASS')
