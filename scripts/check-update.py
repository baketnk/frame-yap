#!/usr/bin/env python3
"""Explicit, bounded GitHub release check. No network access unless --check is passed."""
import argparse
from datetime import datetime
import json
import re
import sys
from urllib import request

RELEASE = re.compile(r"v?((?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.([1-9][0-9]{11}))\Z")
URL = "https://api.github.com/repos/baketnk/frame-yap/releases/latest"


def version(value):
    match = RELEASE.fullmatch(value)
    if not match:
        raise ValueError("invalid release version")
    datetime.strptime(match[2], "%Y%m%d%H%M")
    return tuple(map(int, match[1].split(".")))


def outcome(payload, current):
    data = json.loads(payload)
    tag = data["tag_name"]
    if not isinstance(tag, str) or not tag.startswith("v"):
        raise ValueError("invalid release tag")
    candidate = version(tag)
    return "AVAILABLE " + tag[1:] if candidate > version(current) else "CURRENT"


class NoRedirect(request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


def check(current):
    version(current)  # Reject malformed local metadata before making any request.
    opener = request.build_opener(request.ProxyHandler({}), NoRedirect())
    req = request.Request(URL, headers={"Accept": "application/vnd.github+json", "User-Agent": "FrameYap-update-check"})
    with opener.open(req, timeout=6) as response:
        payload = response.read(65537)
    if len(payload) > 65536:
        raise ValueError("oversized response")
    return outcome(payload, current)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="explicit network check")
    parser.add_argument("--current", required=True)
    args = parser.parse_args(argv)
    if not args.check:
        parser.error("--check required for network access")
    try:
        print(check(args.current), flush=True)
    except (ValueError, KeyError, TypeError, OSError):
        print("ERROR", flush=True)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
