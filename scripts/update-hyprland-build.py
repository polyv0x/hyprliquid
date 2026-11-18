#!/usr/bin/env python3
"""Invalidate adapter objects when their compiler, headers, or ABI changes.

Run on every make invocation, but preserve the stamp's mtime on a no-op. The
preprocessor uses the same include flags as the build (including hyprpm's
headers), never the currently running compositor, which may still be older.
"""

import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    stamp = Path(sys.argv[1])
    compiler = sys.argv[3:]
    if sys.argv[2] != "--" or not compiler:
        raise SystemExit("usage: update-hyprland-build.py STAMP -- COMPILER [FLAGS...]")
    result = subprocess.run(
        [*compiler, "-dM", "-E", "-x", "c++", "-include",
         "hyprland/src/plugins/PluginAPI.hpp", "/dev/null"],
        stdout=subprocess.PIPE,
        check=True,
    )
    digest = hashlib.sha256(json.dumps(compiler).encode() + b"\0" + result.stdout).hexdigest() + "\n"
    if stamp.exists() and stamp.read_text() == digest:
        return
    with tempfile.NamedTemporaryFile(mode="w", dir=stamp.parent, delete=False) as output:
        output.write(digest)
        temporary = Path(output.name)
    temporary.replace(stamp)
    print("Hyprland build configuration changed; rebuilding C++ adapter")


if __name__ == "__main__":
    main()
