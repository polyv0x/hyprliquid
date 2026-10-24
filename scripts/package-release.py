#!/usr/bin/env python3
"""Publish a stripped plugin and its matching debug file without editing the input."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def package(source, output, objcopy):
    output.mkdir(parents=True, exist_ok=True)
    # Stage on the destination filesystem so the complete pair can be renamed
    # atomically. A build-specific directory keeps older debug files available
    # for compositors that still have an older library mapped.
    with tempfile.TemporaryDirectory(prefix='.release-', dir=output) as temporary:
        stage = Path(temporary)
        plugin = stage / 'hyprliquid.so'
        debug = stage / 'hyprliquid.so.debug'
        shutil.copy2(source, plugin)
        source_hash = digest(plugin)
        subprocess.run([objcopy, '--only-keep-debug', str(plugin), str(debug)], check=True)
        subprocess.run([objcopy, '--strip-debug', f'--add-gnu-debuglink={debug}', str(plugin)], check=True)
        manifest = {
            'source_sha256': source_hash,
            'artifacts': {path.name: {'sha256': digest(path), 'bytes': path.stat().st_size}
                          for path in (plugin, debug)},
        }
        (stage / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
        destination = output / source_hash

        def verify_existing():
            # Repeated packaging is idempotent, but never silently accept or
            # overwrite an incomplete/corrupted previously published pair.
            if json.loads((destination / 'manifest.json').read_text()) != manifest:
                raise RuntimeError(f'Release manifest differs: {destination}')
            for name, expected in manifest['artifacts'].items():
                if digest(destination / name) != expected['sha256']:
                    raise RuntimeError(f'Release artifact differs: {destination / name}')

        if destination.exists():
            verify_existing()
        else:
            try:
                stage.rename(destination)
            except OSError:
                # Another packager may have published the same build first.
                if not destination.is_dir():
                    raise
                verify_existing()
        return destination


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--objcopy', default='objcopy')
    args = parser.parse_args()
    destination = package(args.source.resolve(), args.output.resolve(), args.objcopy)
    print(f'Release: {destination / "hyprliquid.so"}')
    print(f'Keep matching symbols: {destination / "hyprliquid.so.debug"}')


if __name__ == '__main__':
    main()
