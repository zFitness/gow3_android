#!/usr/bin/env python3
"""Fetch generic x86-64 Arch libraries; CachyOS AVX-512 builds cannot run in FEX.

Downloads stay local. Never installs or changes host system packages.
"""
import argparse
import concurrent.futures
import hashlib
import json
import subprocess
import tarfile
import urllib.parse
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
MIRROR = 'https://geo.mirror.pkgbuild.com'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--packages', type=Path, default=ROOT / 'tools/android/config/gport-packages.txt')
    parser.add_argument('--cache', type=Path, default=ROOT / '.local-deps/android/arch-db')
    parser.add_argument('--root', type=Path, default=ROOT / '.local-deps/android/baseline')
    args = parser.parse_args()
    args.cache.mkdir(parents=True, exist_ok=True)
    args.root.mkdir(parents=True, exist_ok=True)
    entries = {}
    for repo in ('core', 'extra'):
        database = args.cache / f'{repo}.db'
        if not database.exists():
            urllib.request.urlretrieve(f'{MIRROR}/{repo}/os/x86_64/{repo}.db', database)
        with tarfile.open(database) as archive:
            for member in archive:
                if member.name.endswith('/desc'):
                    text = archive.extractfile(member).read().decode()
                    data = {part.splitlines()[0].strip('%'): '\n'.join(part.splitlines()[1:])
                            for part in text.strip().split('\n\n')}
                    data['repo'] = repo
                    entries[data['NAME']] = data
    packages = sorted(set(args.packages.read_text().splitlines()))
    missing = set(packages) - entries.keys()
    if missing:
        raise SystemExit(f'Packages missing from generic repositories: {sorted(missing)}')

    def fetch(name):
        entry = entries[name]
        path = args.cache / entry['FILENAME']
        if not path.exists():
            url = f"{MIRROR}/{entry['repo']}/os/x86_64/{urllib.parse.quote(path.name)}"
            temp = path.with_suffix(path.suffix + '.part')
            urllib.request.urlretrieve(url, temp)
            temp.rename(path)
        with path.open('rb') as file:
            digest = hashlib.file_digest(file, 'sha256').hexdigest()
        if digest != entry['SHA256SUM']:
            raise RuntimeError(f'Checksum mismatch: {path}')
        return path

    with concurrent.futures.ThreadPoolExecutor(max_workers=6) as pool:
        paths = list(pool.map(fetch, packages))
    for name, path in zip(packages, paths):
        # Keep hard-link targets outside usr/lib (glibc getconf links into usr/bin).
        subprocess.run(['bsdtar', '-xf', str(path), '-C', str(args.root)], check=True)
        print(f'{name}: {entries[name]["VERSION"]}', flush=True)
    (args.root / 'packages.json').write_text(json.dumps(
        {name: entries[name] for name in packages}, indent=2) + '\n')
    print(f'Generic x86-64 libraries: {args.root}')


if __name__ == '__main__':
    main()
