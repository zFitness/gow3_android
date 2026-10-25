#!/usr/bin/env python3
from pathlib import Path
import runpy
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
runpy.run_path(str(Path(__file__).parent / 'build/build_arm64_bundle.py'), run_name='__main__')
