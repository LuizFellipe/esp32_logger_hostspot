#!/usr/bin/env python3
"""Check or synchronize this project's managed Arduino libraries."""

import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import shutil
import sys
import tempfile


def fingerprint(directory):
    if not directory.is_dir():
        return None
    return {
        file.relative_to(directory).as_posix(): hashlib.sha256(file.read_bytes()).hexdigest()
        for file in sorted(directory.rglob('*'))
        if file.is_file()
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    action = parser.add_mutually_exclusive_group()
    action.add_argument('--check', action='store_true', help='Compare files without writing (default).')
    action.add_argument('--apply', action='store_true', help='Copy project libraries to Arduino IDE; preserve replaced copies.')
    parser.add_argument('--ide-dir', type=Path, default=Path.home() / 'Arduino' / 'libraries')
    args = parser.parse_args()
    project = Path(__file__).resolve().parent
    ide = args.ide_dir.expanduser().resolve()
    manifest = json.loads((project / 'manifest.json').read_text())
    entries = manifest['libraries']
    differences = []
    sources = {}
    for entry in entries:
        name = entry['directory']
        if Path(name).name != name or name in ('.', '..'):
            parser.error(f'Invalid library directory: {name!r}')
        source, destination = project / name, ide / name
        if not (source / 'library.properties').is_file():
            parser.error(f'Missing project library: {source}')
        if destination == source or project in destination.parents or destination in project.parents:
            parser.error('IDE destination must be outside the project libraries directory.')
        properties = dict(
            line.split('=', 1) for line in (source / 'library.properties').read_text().splitlines()
            if '=' in line and not line.lstrip().startswith('#')
        )
        if properties.get('name') != entry['name'] or properties.get('version') != entry['version']:
            parser.error(f'{name}: update manifest name/version before synchronizing.')
        sources[name] = fingerprint(source)
        if sources[name] != fingerprint(destination):
            differences.append(name)
    if not differences:
        print(f'{len(entries)} libraries identical: {project} -> {ide}')
        return 0
    if not args.apply:
        print('Libraries differ or are missing: ' + ', '.join(differences), file=sys.stderr)
        return 1
    ide.mkdir(parents=True, exist_ok=True)
    backup = ide.parent / '.esp32-library-backups' / datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S%fZ')
    for name in differences:
        destination = ide / name
        if destination.is_symlink():
            parser.error(f'Refusing to replace symlink: {destination}')
        if destination.exists() and not destination.is_dir():
            parser.error(f'Destination is not a directory: {destination}')
    for name in differences:
        source, destination = project / name, ide / name
        with tempfile.TemporaryDirectory(prefix='.esp32-sync-', dir=ide.parent) as temp:
            staged = Path(temp) / name
            shutil.copytree(source, staged)
            if sources[name] != fingerprint(staged):
                raise RuntimeError(f'Copy verification failed: {name}')
            previous = backup / name
            if destination.exists():
                backup.mkdir(parents=True, exist_ok=True)
                destination.rename(previous)
            try:
                staged.rename(destination)
                if sources[name] != fingerprint(destination):
                    raise RuntimeError(f'IDE verification failed: {name}')
            except Exception:
                if previous.exists() and not destination.exists():
                    previous.rename(destination)
                raise
        print(f'Synchronized {name}')
    print(f'{len(entries)} libraries verified. Replaced copies preserved at {backup}.')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
