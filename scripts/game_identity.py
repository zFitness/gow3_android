#!/usr/bin/env python3
"""Report game identity and patch-profile status without modifying the game directory."""
import argparse
import json
from pathlib import Path

from patches import patch_profile_status


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('game', type=Path, help='player game directory')
    args = parser.parse_args(argv)
    print(json.dumps(patch_profile_status(args.game), indent=2, sort_keys=True))


if __name__ == '__main__':
    main()
