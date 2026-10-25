#!/usr/bin/env python3
"""Fetch a Debian trixie arm64 sysroot for cross-building the native ARM64 runtime.

The Thor's Linux userland (fexdroid's rootfs) is Debian 13 (trixie) arm64: building against the
same release keeps glibc and library ABIs identical. Packages come from deb.debian.org over HTTPS
and every .deb is checked against the SHA-256 in the Packages index. The host is not modified.
"""
import argparse
import copy
import concurrent.futures
import hashlib
import io
import json
import lzma
import os
import re
import shutil
import threading
import tarfile
import time
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
# Overridable: use a mainland mirror by default; override with --mirror or
# GPORT_DEBIAN_MIRROR when another network has better access to Debian upstream.
MIRROR = os.environ.get(
    'GPORT_DEBIAN_MIRROR', 'https://mirrors.tuna.tsinghua.edu.cn/debian')
SUITE = 'trixie'
# What the runtime, the GPU library and FEXCore link against (dependencies follow).
WANTED = [
    'libc6-dev', 'libstdc++-14-dev', 'libgcc-14-dev', 'linux-libc-dev',
    'libvulkan-dev', 'libfmt-dev', 'libxxhash-dev', 'libsdl3-dev', 'libx11-dev', 'libzydis-dev',
    'libavformat-dev', 'libavcodec-dev', 'libavutil-dev', 'libswscale-dev', 'libswresample-dev',
    'spirv-headers', 'zlib1g-dev',
]
# Not needed to compile or link (package management, scripts, documentation, data).
SKIP = re.compile(r'^(debconf|perl.*|dpkg|adduser|tzdata|sensible-utils|libc-bin|libc-dev-bin|'
                  r'fonts-.*|.*-data|.*-doc|ucf|lsb-base|init-system-helpers|'
                  r'python3.*|libpython.*|dbus.*|systemd.*|libsystemd-shared|'
                  r'readline-common|mailcap|media-types|netbase|x11-common|coreutils|bash|'
                  r'base-files|login|util-linux|mount)$')


def fetch(url, attempts=8):
    """Download with retries: a flaky link (TLS EOF, reset) must not abort a multi-file fetch."""
    delay = 2
    for attempt in range(1, attempts + 1):
        try:
            with urllib.request.urlopen(url, timeout=120) as response:
                return response.read()
        except Exception as error:  # noqa: BLE001 - transient network errors only
            if attempt == attempts:
                raise
            print(f'  retry {attempt}/{attempts - 1} for {url.rsplit("/", 1)[-1]}: {error}', flush=True)
            time.sleep(delay)
            delay = min(delay * 2, 30)


def parse_packages(text):
    packages = {}
    for block in text.split('\n\n'):
        fields, key = {}, None
        for line in block.splitlines():
            if line.startswith(' ') and key:
                fields[key] += '\n' + line
            elif ':' in line:
                key, value = line.split(':', 1)
                fields[key] = value.strip()
        if 'Package' in fields:
            packages.setdefault(fields['Package'], fields)
            for provided in fields.get('Provides', '').split(','):
                name = provided.strip().split(' ')[0]
                if name:
                    packages.setdefault('virtual:' + name, fields)
    return packages


def dependencies(fields):
    names = []
    for key in ('Pre-Depends', 'Depends'):
        for group in fields.get(key, '').split(','):
            group = group.strip()
            if group:
                # First alternative, without version or architecture qualifiers.
                names.append(re.split(r'[ (:]', group.split('|')[0].strip())[0])
    return names


def sysroot_member(member, path):
    """Absolute symlinks point into the target system: made relative, inside the sysroot."""
    # Documentation is not needed by the cross-linker and Debian's generated manpage aliases
    # can form long symlink chains while several packages are being merged into one sysroot.
    # Skipping them also keeps Python 3.13's tar_filter from resolving irrelevant links.
    name = member.name.lstrip('./')
    if name.startswith(('usr/share/man/', 'usr/share/doc/', 'usr/share/info/')):
        return None
    if member.issym() and member.linkname.startswith('/'):
        base = os.path.dirname(os.path.normpath(member.name))
        member = copy.copy(member)
        member.linkname = os.path.relpath(member.linkname.lstrip('/'), base or '.')
    # Python 3.12 added TarFile.tar_filter/extractall(filter=...). The macOS
    # Command Line Tools Python 3.9 used by local builds has neither, so keep
    # the same path/symlink checks and return the sanitized member directly.
    tar_filter = getattr(tarfile, 'tar_filter', None)
    return tar_filter(member, path) if tar_filter else member


def extract_deb(data, dest):
    """A .deb is an ar archive; its data.tar.* holds the files."""
    assert data[:8] == b'!<arch>\n', 'not a .deb'
    offset = 8
    while offset < len(data):
        name = data[offset:offset + 16].decode().strip().rstrip('/')
        size = int(data[offset + 48:offset + 58].decode().strip())
        body = data[offset + 60:offset + 60 + size]
        offset += 60 + size + (size & 1)
        if name.startswith('data.tar'):
            with tarfile.open(fileobj=io.BytesIO(body)) as tar:
                try:
                    tar.extractall(dest, filter=sysroot_member)
                except TypeError:
                    for member in tar.getmembers():
                        safe = sysroot_member(member, dest)
                        if safe is not None:
                            tar.extract(safe, dest)
            return
    raise RuntimeError('no data.tar in .deb')


def fetch_cached(item, cache):
    """Download one package into the cache, safely supporting concurrent workers."""
    name, fields = item
    path = cache / Path(fields['Filename']).name
    expected = fields['SHA256']
    if path.exists() and hashlib.sha256(path.read_bytes()).hexdigest() == expected:
        return name, fields, path

    data = fetch(f"{MIRROR}/{fields['Filename']}")
    if hashlib.sha256(data).hexdigest() != expected:
        raise SystemExit(f'SHA-256 mismatch: {name}')

    # Never expose a partial download as a valid cache entry if the process is interrupted.
    temporary = path.with_name(
        f'.{path.name}.{os.getpid()}.{threading.get_ident()}.part')
    temporary.write_bytes(data)
    os.replace(temporary, path)
    return name, fields, path


def overlay_vulkan_headers(source, out):
    """Overlay architecture-independent Vulkan headers newer than Debian's package."""
    source = Path(source)
    include_root = source / 'include'
    if not (include_root / 'vulkan').is_dir():
        raise SystemExit(f'Vulkan headers not found under {source}')
    destination_root = out / 'usr' / 'include'
    destination_root.mkdir(parents=True, exist_ok=True)
    for name in ('vulkan', 'vk_video'):
        include = include_root / name
        if include.is_dir():
            shutil.copytree(include, destination_root / name, dirs_exist_ok=True)


def main():
    global MIRROR
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, default=ROOT / '.local-deps/android/arm64-sysroot')
    parser.add_argument('--packages', nargs='+', default=WANTED,
                        help='packages to fetch instead of the runtime build set (build_turnip.sh)')
    parser.add_argument('--mirror', default=MIRROR,
        help='Debian mirror (default: $GPORT_DEBIAN_MIRROR or Tsinghua mirror)')
    parser.add_argument('--jobs', type=int,
                        default=int(os.environ.get('GPORT_FETCH_JOBS', '8')),
                        help='parallel .deb downloads (default: 8)')
    parser.add_argument('--vulkan-headers', default=os.environ.get('GPORT_VULKAN_HEADERS'),
                        help='optional prefix whose include/vulkan overlays Debian headers')
    args = parser.parse_args()
    if args.jobs < 1:
        raise SystemExit('--jobs must be at least 1')
    MIRROR = args.mirror.rstrip('/')
    out = args.out
    cache = out.parent / 'arm64-debs'
    cache.mkdir(parents=True, exist_ok=True)
    # Cache the release index: it is 9 MB and the answer never changes for a suite. Reusing it
    # means a re-run only needs the .debs it is actually missing.
    index_file = cache / f'Packages-{SUITE}-binary-arm64.xz'
    if index_file.exists():
        index = lzma.decompress(index_file.read_bytes()).decode()
    else:
        raw = fetch(f'{MIRROR}/dists/{SUITE}/main/binary-arm64/Packages.xz')
        index_file.write_bytes(raw)
        index = lzma.decompress(raw).decode()
    packages = parse_packages(index)
    chosen, queue = {}, list(args.packages)
    while queue:
        name = queue.pop()
        if name in chosen or SKIP.match(name):
            continue
        fields = packages.get(name) or packages.get('virtual:' + name)
        if not fields:
            raise SystemExit(f'package not found: {name}')
        if fields['Package'] in chosen or SKIP.match(fields['Package']):
            chosen.setdefault(name, None)
            continue
        chosen[fields['Package']] = fields
        queue.extend(dependencies(fields))
    items = sorted((k, v) for k, v in chosen.items() if v)
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        downloaded = list(pool.map(lambda item: fetch_cached(item, cache), items))

    records = []
    for name, fields, path in downloaded:
        extract_deb(path.read_bytes(), out)
        records.append({'package': name, 'version': fields['Version'], 'sha256': fields['SHA256']})
    if args.vulkan_headers:
        overlay_vulkan_headers(args.vulkan_headers, out)
    # Debian is merged-/usr: absolute paths in linker scripts name /lib and /usr/lib.
    for link in ('lib', 'bin', 'sbin'):
        target = out / link
        if not target.exists() and not target.is_symlink():
            target.symlink_to(f'usr/{link}')
    (out / 'packages.json').write_text(json.dumps(records, indent=2) + '\n')
    print(f'{len(records)} packages in {out}')


if __name__ == '__main__':
    main()
