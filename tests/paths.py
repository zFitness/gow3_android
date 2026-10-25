"""Repository paths for the Python tests (run: python3 -m unittest discover -s tests)."""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
if str(ROOT / 'scripts') not in sys.path:
    sys.path.insert(0, str(ROOT / 'scripts'))
# The runtime manifest composer lives beside the manifest it composes.
if str(ROOT / 'tools/android/runtime') not in sys.path:
    sys.path.insert(0, str(ROOT / 'tools/android/runtime'))
