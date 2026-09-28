#!/usr/bin/env python3
"""Attended terminal handoff for a user-requested FrameYap update."""
import argparse
from pathlib import Path
import subprocess
import sys

# The same validation as check-update.py, without importing the network helper.
import re
VERSION = re.compile(r"(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.[1-9][0-9]{11}\Z")


def installer_command(script, version):
    if not VERSION.fullmatch(version):
        raise ValueError("invalid version")
    return ["sh", str(script), "--mode", "binary", "--version", version, "--yes"]


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--version", required=True)
    args = parser.parse_args(argv)
    script = Path(__file__).resolve().parent.parent / "bin/install.sh"
    try:
        command = installer_command(script, args.version)
    except ValueError:
        parser.error("invalid release version")
    if not script.is_file():
        print("Installed FrameYap installer not found.")
        return 1
    print(f"FrameYap update: {args.version}")
    print("Close FrameYap, then press Enter here to download and install this version.")
    if not sys.stdin.isatty():
        print("A terminal is required; no download started.")
        return 1
    try:
        input()
    except (KeyboardInterrupt, EOFError):
        print("Update cancelled.")
        return 1
    result = subprocess.run(command, check=False)
    print("Update installed." if result.returncode == 0 else "Update failed; check the installer message above.")
    return result.returncode


if __name__ == "__main__":
    sys.exit(main())
