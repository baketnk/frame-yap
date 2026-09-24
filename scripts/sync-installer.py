#!/usr/bin/env python3
"""Developer helper: synchronize the standalone piped installer payload."""
from pathlib import Path
root = Path(__file__).resolve().parents[1]
wrapper = root / "install.sh"
header = wrapper.read_text().split("<<'PY'\n", 1)[0] + "<<'PY'\n"
payload = (root / "scripts/install_payload.py").read_text()
if "\nPY\n" in payload:
    raise ValueError("heredoc delimiter appears in payload")
wrapper.write_text(header + payload + "\nPY\n")
