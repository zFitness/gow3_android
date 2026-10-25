#!/usr/bin/env python3
"""Stage the existing x86-64 build for a FEX Android boot experiment.

This is not an ARM port or a standalone APK. Game files stay outside the bundle.
Only run against trusted local ELF binaries: dependency discovery uses ldd.
"""
import argparse
import hashlib
import os
import json
import re
import shutil
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT))

from tools.android.runtime.compose_manifest import compose


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, default=ROOT / 'out/thor/bundle')
    parser.add_argument('--probe', type=Path, default=ROOT / 'out/thor/gport-probe',
                        help='loader linked with generic x86-64 startup objects')
    parser.add_argument('--baseline-root', type=Path,
                        help='generic x86-64 libraries to replace optimized host packages')
    args = parser.parse_args()
    game_dir = os.environ.get('SPARTAN_GAME_DIR')
    if not game_dir:
        raise SystemExit('SPARTAN_GAME_DIR is required (select the intended God of War III dump)')
    stage = args.out.resolve()
    lib = stage / 'rootfs/usr/lib'
    lib.mkdir(parents=True, exist_ok=True)
    (stage / 'bin').mkdir(exist_ok=True)
    (stage / 'data').mkdir(exist_ok=True)
    sources = [args.probe.resolve(), ROOT / 'out/gpu/libgportgpu.so']
    dependencies = {}
    for source in sources:
        output = subprocess.check_output(['ldd', str(source)], text=True)
        if 'not found' in output:
            raise SystemExit(f'Unresolved dependencies for {source}:\n{output}')
        for line in output.splitlines():
            match = re.search(r'=> (/\S+)', line) or re.match(r'\s*(/\S+)', line)
            if match:
                path = Path(match[1])
                dependencies[path.name] = path
    for name, source in sorted(dependencies.items()):
        if name == 'libgportgpu.so':
            continue
        if args.baseline_root and (args.baseline_root / 'usr/lib' / name).exists():
            source = args.baseline_root / 'usr/lib' / name
        elif args.baseline_root:
            raise SystemExit(f'Generic dependency missing: {name}; refusing optimized host fallback')
        shutil.copy2(source, lib / name)
    shutil.copy2(sources[0], stage / 'bin/gport-probe')
    for name in ('renderer-probe', 'controller-probe', 'bb-gpu-capabilities'):
        helper = sources[0].parent / name
        if helper.exists():
            shutil.copy2(helper, stage / 'bin' / name)
    shutil.copy2(sources[1], lib / 'libgportgpu.so')
    for path in [*stage.joinpath('bin').iterdir(), *lib.iterdir()]:
        subprocess.run(['strip', '--strip-debug', str(path)], check=True)
        # Every dependency can find its siblings. Leave Vulkan's standard SONAME
        # intact so the FEX runtime can replace it with its native ARM thunk.
        if not path.name.startswith('ld-linux-'):
            subprocess.run(['patchelf', '--set-rpath', '$ORIGIN:$ORIGIN/../rootfs/usr/lib', str(path)], check=True)
    for name in ('lib', 'lib64'):
        path = stage / 'rootfs' / name
        if not path.exists():
            path.symlink_to('usr/lib')
    shutil.copy2(ROOT / 'out/boot-linked.bin', stage / 'data/boot-linked.bin')
    shutil.copy2(ROOT / 'out/content.bin', stage / 'data/content.bin')
    # 载荷里的文件名由运行时清单声明：bundle 不自带名字。
    manifest = compose('release')
    shutil.copy2(ROOT / 'tools/android/config' / manifest['default_config_name'],
                   stage / manifest['config_name'])
    shutil.copy2(ROOT / 'tools/android/device' / manifest['launch_script'],
                   stage / manifest['launch_script'])
    shutil.copy2(ROOT / 'tools/android/config' / manifest['pad_db_name'],
                   stage / manifest['pad_db_name'])
    shutil.copy2(ROOT / 'tools/android/config' / manifest['fex_config_name'],
                   stage / manifest['fex_config_name'])
    elf = stage / 'data/eboot.elf'
    elf.symlink_to(ROOT / 'out/eboot.elf')
    try:
        subprocess.run([
            'python3', str(ROOT / 'scripts/patches.py'), '--out', str(stage / 'data'),
            '--fps', '30', '--settings', str(stage / manifest['config_name']),
            '--game-dir', game_dir,
            '--render-res', '640x360', '--output-res', '1280x720',
        ], check=True)
    finally:
        elf.unlink()
    # A small imported-call fixture verifies execution of mapped x86 game code,
    # not just that the FEX executable or the Linux loader can start.
    code = bytes.fromhex('ff25020000009090').ljust(4096, b'\0')
    fixture = (struct.pack('<8s5Q', b'BBPROBE1', len(code), 0, 1, 1, 1)
               + struct.pack('<3Q', 0, len(code), 5)
               + b'thor-cpu-fixture'.ljust(128, b'\0')
               + struct.pack('<2Q2q', 8, 1, 0, 0) + code)
    (stage / 'data/cpu-fixture.bin').write_bytes(fixture)
    records = []
    for path in sorted(stage.rglob('*')):
        if path.is_file() and not path.is_symlink() and path.name != 'manifest.json':
            with path.open('rb') as file:
                digest = hashlib.file_digest(file, 'sha256').hexdigest()
            records.append({'path': str(path.relative_to(stage)), 'bytes': path.stat().st_size,
                            'sha256': digest})
    (stage / 'manifest.json').write_text(json.dumps(records, indent=2) + '\n')
    print(f'Staged {len(records)} files, {sum(r["bytes"] for r in records) / 2**20:.1f} MiB: {stage}')


if __name__ == '__main__':
    main()
