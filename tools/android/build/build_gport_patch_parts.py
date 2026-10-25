#!/usr/bin/env python3
"""Pre-compile the game patches the Thor's graphics settings can choose, one file per choice.

The Thor has no Python: scripts/patches.py cannot run at each start as in run.sh. Its patches are
compiled here instead, a BBPATCH2 file per effect switch, model LOD and scene resolution, and
run-gport.sh passes the loader the files gport.ini selects (the loader applies them in order).
fps=uncap.bin: the frame rate patches for any limit above the game's 30.
"""
import argparse
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'scripts'))
import patches  # noqa: E402

# Outputs the Thor offers (its screen is 1920x1080): the upscaler's output, the game's UI size.
OUTPUTS = [(1280, 720), (1920, 1080)]


def render_sizes():
    """Scene sizes of every preset at every output (patches.scaled_sizes), the game's own excluded."""
    sizes = set()
    for out in OUTPUTS:
        for scale in patches.PRESET_SCALES:
            size = tuple(max(2, round(v / scale / 2) * 2) for v in out)
            if size == patches.OUTPUT_SIZE:
                continue  # 1920x1080: the game's own size, no patch
            sizes.add(size)
    return sorted(sizes)


def write(path, writes):
    blob = struct.pack('<8sQQ', b'BBPATCH2', patches.EBOOT_BASE, len(writes))
    for offset, data in writes:
        blob += struct.pack('<QQ', offset, len(data)) + data
    path.write_bytes(blob)


# Cheats (the app's Cheats menu), as code hooks of 1.09 always in place and switched live: each
# hook's code tests its flag byte, which the loader keeps as GPORT_CHEATS_FILE says (probe.c), and does
# the game's own instructions when it is 0. The code goes in the unused end of the code segment's
# last page (past the segment's 0x50d96dc bytes, mapped executable with it), the flags in the
# unused end of the data segment's (past 0x56d30f4, mapped writable). The same hooks as
# Shiningami's GoldHEN cheats, re-written to stay inside the image.
CAVE = 0x50d9700         # code segment end 0x50d96dc; its page ends at 0x50da000
CHEAT_FLAGS = 0x56d3f00  # data segment end 0x56d30f4; its page ends at 0x56d4000 (probe.c too)


def jump(at, to):
    return b'\xe9' + struct.pack('<i', to - (at + 5))


def flag_test(at, flag):
    """cmp byte [rip + flag], 0 at `at` (7 bytes)."""
    return b'\x80\x3d' + struct.pack('<i', flag - (at + 7)) + b'\x00'


def cheat_writes(text):
    """The hooks and their code, checked against this eboot's 1.09 code."""
    writes = []
    # Health: the player's HP (+0xf8) read each frame for the HUD; flag on: the max HP (+0xfc) first.
    site, original, back, cave = 0x18f78b1, bytes.fromhex('8b88f8000000'), 0x18f78b7, CAVE
    code = flag_test(cave, CHEAT_FLAGS) + bytes.fromhex(
        '740c'                        # je +12 (off)
        '8b88fc000000' '8988f8000000' # mov ecx,[rax+0xfc]; mov [rax+0xf8],ecx
        '8b88f8000000')               # mov ecx,[rax+0xf8] (the game's own)
    writes.append((site, original, back, cave, code, b''))
    # Items: the count change (count += amount, at 0x14d94a0); flag on: a negative amount (a blood
    # vial or bullet used, items given away) leaves the count as it was.
    site, original, back, cave = 0x14d9540, bytes.fromhex('448b63084501f44139c4'), 0x14d954a, CAVE + 0x40
    code = bytes.fromhex('448b6308') + flag_test(cave + 4, CHEAT_FLAGS + 1) + bytes.fromhex(
        '7405'                        # je +5 (off: add)
        '4585f6' '7803'               # test r14d,r14d; js +3 (skip the add)
        '4501f4' '4139c4')            # add r12d,r14d; cmp r12d,eax (the game's own)
    writes.append((site, original, back, cave, code, b''))
    out = []
    for site, original, back, cave, code, _ in writes:
        if text[site:site + len(original)] != original:
            raise SystemExit(f'cheats: unexpected code at {site:#x}: not the 1.09 eboot')
        code += jump(cave + len(code), back)
        hook = jump(site, cave) + b'\x90' * (len(original) - 5)
        assert cave + len(code) <= 0x50da000
        out += [(cave, code), (site, hook)]
    return out


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True, help='directory for the patch files')
    parser.add_argument('--eboot', type=Path, default=ROOT / 'out/eboot.elf')
    parser.add_argument('--xml', type=Path, default=ROOT / 'patches/God of War III.xml')
    parser.add_argument('--app-version', default='01.09')
    args = parser.parse_args()
    segments = patches.eboot_segments(args.eboot.read_bytes())
    args.out.mkdir(parents=True, exist_ok=True)
    count = 0
    for key, choices in patches.EFFECTS.items():
        for value, name in (('0', choices[0]), ('1', choices[1])):
            write(args.out / f'{key}={value}.bin',
                  patches.compile_patches(args.xml, [name], args.app_version, segments) if name else [])
            count += 1
    for value, name in patches.MODEL_LOD.items():
        write(args.out / f'model_lod={value}.bin',
              patches.compile_patches(args.xml, [name], args.app_version, segments))
        count += 1
    # Above 30 FPS: the delta-time patch with the sprint fix (patches.FPS_PRESETS['uncap']); the
    # present thread then limits the rate (SPARTAN_FPS_LIMIT, run-gport.sh).
    write(args.out / 'fps=uncap.bin',
          patches.compile_patches(args.xml, patches.FPS_PRESETS['uncap'], args.app_version, segments))
    count += 1
    # The title screen straight to the main menu, offline (on unless skip_network_choice=0).
    write(args.out / 'skip_network_choice=1.bin',
          patches.compile_patches(args.xml, [patches.SKIP_NETWORK_CHOICE], args.app_version, segments))
    count += 1
    text = args.eboot.read_bytes()[0x4000:0x4000 + 0x50d96dc]  # the code segment (offset 0x4000, vaddr 0)
    write(args.out / 'cheats.bin', cheat_writes(text))
    count += 1
    for size in render_sizes():
        writes = patches.resolution_writes(args.xml, size, args.app_version, segments)
        write(args.out / f'res-{size[0]}x{size[1]}.bin', writes)
        count += 1
    print(f'{count} patch files in {args.out}')


if __name__ == '__main__':
    main()
