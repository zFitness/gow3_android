#!/usr/bin/env python3
"""Compose the runtime manifest for one build profile.

The layout is declared once, in ``runtime-manifest.json``.  A profile only states what
differs from it (today: the profile name and its note) in
``runtime-manifest.<profile>.json``.  Two hand-maintained copies of the layout used to
drift apart -- the debug copy was missing two required scripts -- so the copies are gone:
everything that is not profile-specific comes from the single template.
"""
import argparse
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
TEMPLATE = ROOT / 'tools/android/runtime/runtime-manifest.json'


class ManifestError(ValueError):
    pass


def _load(path):
    try:
        return json.loads(Path(path).read_text())
    except (OSError, json.JSONDecodeError) as error:
        raise ManifestError('cannot read %s: %s' % (path, error)) from error


def compose(profile, template=TEMPLATE):
    """Return the manifest for ``profile``: the template plus that profile's overrides."""
    manifest = _load(template)
    override_path = Path(template).with_name('runtime-manifest.%s.json' % profile)
    if override_path.is_file():
        overrides = _load(override_path)
        for key, value in overrides.items():
            manifest[key] = value
    manifest['profile'] = profile
    for key in ('runtime_root', 'port_root', 'port_arch_root', 'home_root',
                'guest_home_root', 'data_root', 'config_name', 'launch_script',
                'default_config_name', 'fex_config_name', 'pad_db_name',
                'top_level_roots', 'required_directories',
                'required_paths', 'entrypoints'):
        if key not in manifest:
            raise ManifestError('the manifest template is missing %r' % key)
    return manifest


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--profile', choices=('release', 'debug'), default='release')
    parser.add_argument('--out', type=Path, default=None,
                        help='where to write the composed manifest (default: stdout)')
    args = parser.parse_args(argv)
    try:
        manifest = compose(args.profile)
    except ManifestError as error:
        parser.error(str(error))
    text = json.dumps(manifest, indent=2, sort_keys=False) + '\n'
    if args.out is None:
        print(text, end='')
        return
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(text)
    print('runtime manifest (%s): %s' % (args.profile, args.out))


if __name__ == '__main__':
    main()
