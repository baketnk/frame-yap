#!/usr/bin/env python3
"""Compatibility entry point for scripts/stage-native.py."""
from pathlib import Path
import runpy

runpy.run_path(str(Path(__file__).with_name('stage-native.py')), run_name='__main__')
