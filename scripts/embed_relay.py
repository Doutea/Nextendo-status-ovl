#!/usr/bin/env python3
"""Generate a private header from a GitHub Actions secret, never print its value."""
import json
import os
from pathlib import Path
import re
import sys

def render(key):
    if not re.fullmatch(r"[A-Za-z0-9._-]{32,128}", key):
        raise ValueError("NEXTENDO_RELAY_KEY must be 32-128 letters, digits, dots, underscores or hyphens")
    config = ("relay_url=https://relay.doutea.cloud\n"
              "relay_ip=104.21.62.41\n"
              "relay_ip_backup=172.67.219.244\n"
              "relay_key=" + key + "\n")
    return "#pragma once\n#define NEXTENDO_EMBEDDED_RELAY_CONFIG " + json.dumps(config) + "\n"

def main():
    target = Path(__file__).resolve().parent.parent / "source/relay_embedded.generated.hpp"
    key = os.environ.get("NEXTENDO_RELAY_KEY", "")
    # Never let an earlier secret-bearing header survive a different build.
    target.unlink(missing_ok=True)
    try:
        text = render(key)
    except ValueError as error:
        print(str(error), file=sys.stderr)
        return 1
    target.write_text(text, encoding="utf-8")
    print("Embedded relay configuration prepared; key omitted from output.")
    return 0

if __name__ == "__main__":
    sys.exit(main())
