#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Build target flags of this tree: the Python side of the CMake options SPARTAN_ARM and
SPARTAN_NIXPORT (gpu/CMakeLists.txt).

Python has no compile step, so the build line that turns the CMake options on writes the same
flags into config/gport-build-flags.json in the program tree (packaging/arm-linux/build.sh does
it). A tree without that file is a desktop build: both flags are off and every desktop default
stays as it is. The flags never change while the tree runs.
"""
import json
from pathlib import Path

# The program tree: scripts/'s parent. The launcher and run.py both live one level below it.
TREE = Path(__file__).resolve().parent.parent
FLAGS_FILE = TREE / 'config' / 'gport-build-flags.json'


def load(path=None):
    """The flags of a build: {'arm': bool, 'nixport': bool}. A missing or broken file means a
    desktop build: both flags off."""
    try:
        data = json.loads(Path(path or FLAGS_FILE).read_text(encoding='utf-8'))
    except (OSError, ValueError):
        return {'arm': False, 'nixport': False}
    if not isinstance(data, dict):
        return {'arm': False, 'nixport': False}
    return {'arm': bool(data.get('arm')), 'nixport': bool(data.get('nixport'))}


FLAGS = load()

# ARM: the binary targets ARM. ARM Linux and Android share one build, so the GPU code that
# differs on ARM branches on this.
ARM = FLAGS['arm']

# Nixport: the binary targets the Nixport platform: the host service bus (toasts, dialogs, the
# file picker) and the mobile styles.
NIXPORT = FLAGS['nixport']
