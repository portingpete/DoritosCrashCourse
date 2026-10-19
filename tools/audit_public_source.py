"""Audit the public source allowlist, credentials and optional original-file hashes."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
DIRECTORIES = {'app', 'runtime', 'renderer', 'config', 'tools', 'tests', 'assets', 'docs', '.github'}
FILES = {'.gitignore', '.gitattributes', 'CMakeLists.txt', 'COPYING', 'README.md', 'CHANGELOG.md',
         'Launch.cmd', 'Character Importer.cmd', 'game/.gitkeep'}
EXTENSIONS = {'.cpp', '.h', '.py', '.ps1', '.toml', '.md', '.txt', '.json', '.patch', '.png', '.cmf',
              '.yml', '.yaml', '.cmd'}
SECRETS = [re.compile(pattern.encode()) for pattern in (
    r'gh[pousr]_[A-Za-z0-9]{30,}', r'github_pat_[A-Za-z0-9_]{40,}',
    r'sk-(?:proj-|ant-)[A-Za-z0-9_-]{20,}', r'AKIA[0-9A-Z]{16}',
    r'-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----')]


def allowed(name: str) -> bool:
    path = Path(name)
    return name in FILES or (len(path.parts) > 1 and path.parts[0] in DIRECTORIES
                            and path.suffix.lower() in EXTENSIONS
                            and '__pycache__' not in path.parts)


def candidates() -> list[str]:
    result = [name for name in FILES if (ROOT / name).is_file()]
    stack = [ROOT / name for name in sorted(DIRECTORIES) if (ROOT / name).exists()]
    while stack:
        folder = stack.pop()
        if folder.is_symlink() or (hasattr(folder, 'is_junction') and folder.is_junction()):
            raise ValueError(f'Linked public directory: {folder.relative_to(ROOT)}')
        for item in sorted(folder.iterdir()):
            if item.name == '__pycache__':
                continue
            if item.is_symlink() or (hasattr(item, 'is_junction') and item.is_junction()):
                raise ValueError(f'Linked public path: {item.relative_to(ROOT)}')
            if item.is_dir():
                stack.append(item)
            elif item.suffix != '.pyc':
                result.append(item.relative_to(ROOT).as_posix())
    return sorted(set(result))


def audit(names: list[str], pak: Path | None = None, staged=False) -> dict:
    failures, hashes, total = [], {}, 0
    indexed = {}
    if staged:
        records = subprocess.check_output(['git', 'ls-files', '--stage', '-z'], cwd=ROOT).split(b'\0')
        entries = {}
        for record in filter(None, records):
            metadata, name = record.split(b'\t', 1)
            mode, sha, stage = metadata.split()
            if stage != b'0':
                raise ValueError('Unmerged paths in the publication index')
            entries[name.decode()] = (mode, sha)
        object_ids = sorted({entries[name][1] for name in names})
        payload = subprocess.check_output(['git', 'cat-file', '--batch'], cwd=ROOT,
                                          input=b'\n'.join(object_ids) + b'\n')
        position, objects = 0, {}
        for expected in object_ids:
            end = payload.index(b'\n', position)
            sha, kind, size = payload[position:end].split()
            if sha != expected or kind != b'blob':
                raise ValueError('Unexpected staged object in publication audit')
            position = end + 1
            size = int(size)
            objects[sha] = payload[position:position + size]
            position += size + 1
        indexed = {name: (mode, objects[sha]) for name, (mode, sha) in entries.items()}
    originals = set()
    if pak:
        from inspect_pak import read_directory, read_entry
        entries = read_directory(pak)
        originals.update(hashlib.sha256(read_entry(pak, item)).hexdigest() for item in entries)
        originals.add(hashlib.sha256(pak.read_bytes()).hexdigest())
        xex = pak.parent / 'default.xex'
        if xex.is_file():
            originals.add(hashlib.sha256(xex.read_bytes()).hexdigest())
    for name in names:
        if not allowed(name):
            failures.append(f'Outside public source allowlist: {name}')
            continue
        path = ROOT / name
        if staged:
            mode, data = indexed[name]
            if mode == b'120000':
                failures.append(f'Linked staged path: {name}')
                continue
        else:
            if path.is_symlink() or (hasattr(path, 'is_junction') and path.is_junction()):
                failures.append(f'Linked public file: {name}')
                continue
            data = path.read_bytes()
        total += len(data)
        sha = hashlib.sha256(data).hexdigest()
        hashes[name] = sha
        if len(data) > 20 * 1024 * 1024:
            failures.append(f'Oversized public file: {name}')
        if sha in originals and data:
            failures.append(f'Byte-identical to an original game file/entry: {name}')
        if path.suffix.lower() not in {'.cmf', '.png'} and any(pattern.search(data) for pattern in SECRETS):
            failures.append(f'Credential/private-key signature: {name}')
        if path.suffix.lower() == '.cmf' and name != 'assets/characters/cj/model.cmf':
            failures.append(f'Unreviewed binary mesh: {name}')
        if path.suffix.lower() == '.png' and not name.startswith(('assets/input-prompts/', 'assets/characters/cj/')):
            failures.append(f'Unreviewed image: {name}')
    return {'passed': not failures, 'files': len(hashes), 'bytes': total,
            'compared_to_originals': bool(pak), 'failures': failures, 'sha256': hashes}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pak', type=Path)
    parser.add_argument('--staged', action='store_true', help='Audit exact index contents instead of candidate files')
    parser.add_argument('--list', action='store_true', help='Print candidate paths, one per line')
    parser.add_argument('--report', type=Path)
    args = parser.parse_args()
    names = (subprocess.check_output(['git', 'ls-files', '-z'], cwd=ROOT).decode().strip('\0').split('\0')
             if args.staged else candidates())
    if args.list:
        print('\n'.join(names))
        return 0
    result = audit(names, args.pak, args.staged)
    if args.report:
        args.report.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({key: value for key, value in result.items() if key != 'sha256'}, indent=2))
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        print('Public source audit: ' + str(error), file=sys.stderr)
        raise SystemExit(1)
