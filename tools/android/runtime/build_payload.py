#!/usr/bin/env python3
"""Compatibility entrypoint for the authoritative Android runtime assembler.

The old assembler produced ``<port>/bin`` and ``<port>/lib`` while the
release installer consumes ``<port>/arm64``. Keep this command for existing
local build notes, but delegate it so it cannot create a second archive schema.
"""
import runpy
from pathlib import Path


if __name__ == '__main__':
    runpy.run_path(str(Path(__file__).with_name('build_release_runtime.py')), run_name='__main__')
