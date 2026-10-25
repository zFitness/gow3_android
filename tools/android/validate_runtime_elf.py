#!/usr/bin/env python3
from pathlib import Path
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.android.runtime.validate_runtime_elf import *

if __name__ == '__main__':
    from tools.android.runtime.validate_runtime_elf import main
    main()
