#!/usr/bin/env python3
"""Compatibility commands for tools/board.py; launch admission belongs to that runner."""
import sys
from pathlib import Path

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))


def main(argv=None):
    import board
    return board.development(argv)


if __name__ == '__main__':
    sys.exit(main())
