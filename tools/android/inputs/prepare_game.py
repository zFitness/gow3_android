#!/usr/bin/env python3
"""Build the runtime image from the player's own God of War III v1.09 files.

The release APK contains no game executable, module, patch image, or save.
"""
import json
import os
import shutil
import sys
from pathlib import Path

here = Path(__file__).resolve().parent
if not (here / 'prepare.py').is_file():
    sys.path.insert(0, str(here.parents[1] / 'scripts'))

import content_profile
import link_modules
import prepare


def signature(game):
    result = {'path': str(game.resolve())}
    for name in ('eboot.bin', 'sce_module/libc.prx', 'sce_module/libSceFios2.prx',
                 'sce_sys/param.sfo'):
        st = (game / name).stat()
        result[name] = [st.st_size, st.st_mtime_ns]
    return result


def main():
    if len(sys.argv) != 3:
        raise SystemExit('usage: prepare_game.py GAME_DIR DATA_DIR')
    game, data = Path(sys.argv[1]), Path(sys.argv[2])
    data.mkdir(parents=True, exist_ok=True)
    identity = signature(game)
    marker = data / 'game-source.json'
    if (data / 'boot-linked.bin').is_file() and (data / 'content.bin').is_file() and marker.is_file():
        try:
            if json.loads(marker.read_text()) == identity:
                print('Game image: existing image matches selected game', flush=True)
                return
        except (OSError, ValueError):
            pass
    work = data / 'prepare-tmp'
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir()
    try:
        print('Game image: preparing selected game files (first launch may take a few minutes)', flush=True)
        prepare.prepare(game, work)
        link_modules.link(game, work)
        content_profile.prepare(game, work)
        os.replace(work / 'boot-linked.bin', data / 'boot-linked.bin')
        os.replace(work / 'content.bin', data / 'content.bin')
        marker.write_text(json.dumps(identity, sort_keys=True) + '\n')
        print('Game image: ready', flush=True)
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == '__main__':
    main()
