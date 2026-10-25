#!/usr/bin/env python3
"""Write a small Android-side texture-integrity diagnostic record.

Only runtime metadata is written. The game directory and game contents are never copied.
"""
import argparse
import json
import os
import platform
import subprocess
from pathlib import Path


def _command(*args):
    try:
        return subprocess.check_output(args, text=True, stderr=subprocess.DEVNULL).strip()
    except (OSError, subprocess.CalledProcessError):
        return None


def record(path, profile, env=None, turnip=None, dmem=None, vblank=None, vulkan_status=None):
    env = os.environ if env is None else env
    data = {
        'schema': 'gow3.texture-integrity.v1',
        'title_id': profile.get('title_id'),
        'app_version': profile.get('app_version'),
        'eboot_sha256': profile.get('eboot_sha256'),
        'patch_profile_supported': profile.get('supported'),
        'patch_profile_status': profile.get('reason'),
        'patch_phase': profile.get('phase'),
        'compiled_patches': profile.get('applied', []),
        'skipped_patches': profile.get('skipped', {}),
        'effective_patch_parameters': profile.get('effective', {}),
        'patch_write_count': profile.get('write_count'),
        'patches_sha256': profile.get('patches_sha256'),
        'vulkan_status': vulkan_status if vulkan_status is not None
                         else env.get('SPARTAN_VULKAN_STATUS', 'unknown'),
        'upscaler': env.get('SPARTAN_UPSCALER', 'none'),
        'upscaler_source': env.get('SPARTAN_UPSCALER_SOURCE', 'unknown'),
        'readback_linear': env.get('SPARTAN_READBACK_LINEAR', '1'),
        'readback_linear_source': env.get('SPARTAN_READBACK_LINEAR_SOURCE', 'unknown'),
        'deferred_readback': env.get('SPARTAN_DEFERRED_READBACK'),
        'stale_readback': env.get('SPARTAN_STALE_READBACK'),
        'render_resolution': profile.get('effective', {}).get(
            'render_resolution', env.get('SPARTAN_RENDER_RES', 'native')),
        'dmem_mb': int(dmem) if dmem and str(dmem).isdigit() else dmem,
        'vblank_hz': int(vblank) if vblank and str(vblank).isdigit() else vblank,
        'turnip': turnip,
        'platform': platform.platform(),
        'gpu_chip_id': _command('kgsl-chip-id') or _command('getprop', 'ro.hardware.egl'),
        'shader_cache_state': env.get('GPORT_SHADER_CACHE_STATE', 'unknown'),
        'capture_id': env.get('SPARTAN_CAPTURE_ID'),
        'capture_dir': env.get('SPARTAN_CAPTURE_DIR'),
    }
    output = Path(path)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(data, indent=2, sort_keys=True) + '\n', encoding='utf-8')
    return data


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', required=True, type=Path)
    parser.add_argument('--profile', required=True, type=Path)
    parser.add_argument('--turnip')
    parser.add_argument('--dmem')
    parser.add_argument('--vblank')
    parser.add_argument('--vulkan-status', choices=['ok', 'failed', 'unknown'], default=None)
    args = parser.parse_args(argv)
    profile = json.loads(args.profile.read_text(encoding='utf-8'))
    record(args.out, profile, turnip=args.turnip, dmem=args.dmem, vblank=args.vblank,
           vulkan_status=args.vulkan_status)


if __name__ == '__main__':
    main()
