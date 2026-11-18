#!/usr/bin/env python3
"""Build and exercise isolated plugin copies against supported Hyprland installs.

Example: --target 0.56.2=/tmp/hyprland-056-root --target 0.57.2=/
Each root contains usr/bin/Hyprland, matching headers, and matching dependencies.
Package archives may be extracted into a root; no system installation is needed.
"""

import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent


def target_environment(root, metadata):
    env = os.environ.copy()
    for name in ('CARGO_TARGET_DIR', 'PKG_CONFIG_PATH', 'PKG_CONFIG_SYSROOT_DIR',
                 'PKG_CONFIG_LIBDIR', 'LD_LIBRARY_PATH', 'HYPRLIQUID_TEST_PLUGIN',
                 'HYPRLIQUID_TEST_OUTPUT', 'HYPRLIQUID_TEST_SECURITY_PLUGIN'):
        env.pop(name, None)
    env['PATH'] = str(root / 'usr/bin') + os.pathsep + env['PATH']
    if root != Path('/'):
        metadata.mkdir()
        # Extracted .pc files often retain /usr. Rebase private copies so both
        # compiler and linker use the chosen version, without redirecting all
        # unrelated system dependencies through an incomplete sysroot.
        for directory in ('usr/lib/pkgconfig', 'usr/share/pkgconfig'):
            for source in (root / directory).glob('*.pc'):
                data = re.sub(r'(?m)^(prefix|includedir|libdir)=/usr(?=/|$)',
                              lambda match: match[1] + '=' + str(root / 'usr'), source.read_text())
                (metadata / source.name).write_text(data)
        env['PKG_CONFIG_PATH'] = str(metadata)
        env['LD_LIBRARY_PATH'] = str(root / 'usr/lib')
    return env


def copy_source(destination):
    for name in ('src', 'crates', 'scripts', 'materials', 'shaders', 'tests', '.cargo'):
        shutil.copytree(ROOT / name, destination / name, ignore=shutil.ignore_patterns('__pycache__'))
    for name in ('Makefile', 'Cargo.toml', 'Cargo.lock', 'rust-toolchain.toml'):
        shutil.copy2(ROOT / name, destination / name)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--target', action='append', required=True, metavar='VERSION=ROOT')
    parser.add_argument('--output', type=Path, help='new directory for logs and screenshots')
    parser.add_argument('--jobs', type=int, default=4)
    args = parser.parse_args()
    output = args.output.resolve() if args.output else Path(tempfile.mkdtemp(prefix='hyprliquid-compat-results-'))
    if args.output:
        output.mkdir(parents=True, exist_ok=False)
    print('Compatibility results:', output, flush=True)
    reports = []
    for specification in args.target:
        version, separator, path = specification.partition('=')
        if not separator or not re.fullmatch(r'[0-9]+\.[0-9]+\.[0-9]+', version):
            parser.error('--target must be VERSION=ROOT')
        root = Path(path).resolve(strict=True)
        result = output / version
        result.mkdir()
        with tempfile.TemporaryDirectory(prefix='hyprliquid-compat-build-') as temporary:
            source = Path(temporary)
            copy_source(source)
            env = target_environment(root, source / 'pkgconfig')
            headers = subprocess.check_output(['pkg-config', '--modversion', 'hyprland'], env=env, text=True).strip()
            assert headers == version, f'{version}: pkg-config selected {headers}'
            runtime = subprocess.check_output(['Hyprland', '--version'], env=env, text=True)
            assert re.search(r'Hyprland\s+' + re.escape(version) + r'\b', runtime), runtime
            (result / 'version.txt').write_text(runtime)

            def run(log_name, command, extra=None):
                with (result / log_name).open('w') as log:
                    try:
                        subprocess.run(command, cwd=source, env=env | (extra or {}), stdout=log,
                                       stderr=subprocess.STDOUT, check=True)
                    except subprocess.CalledProcessError:
                        print((result / log_name).read_text()[-8000:], flush=True)
                        raise

            run('build.log', ['make', f'-j{args.jobs}', 'all', 'test',
                              'build/layer-test', 'build/decoration-lifecycle-test.so'])
            artifacts = [*sorted((source / 'build').glob('*.o')), source / 'hyprliquid.so',
                         source / 'build/hyprland-build.stamp']
            before = [item.stat().st_mtime_ns for item in artifacts]
            run('noop-build.log', ['make', f'-j{args.jobs}', 'all'])
            assert before == [item.stat().st_mtime_ns for item in artifacts], 'No-op build rebuilt adapter'
            for name, option in [('render', '--compatibility-only'), ('lifecycle', '--decoration-lifecycle-only')]:
                run(name + '.log', ['python3', 'scripts/test-nested.py', option],
                    {'HYPRLIQUID_TEST_OUTPUT': str(result / name)})
            reports.append({'version': version, 'headers': headers, 'root': str(root), 'passed': True})
            (output / 'results.json').write_text(json.dumps(reports, indent=2) + '\n')
            print(f'PASS: Hyprland {version} build, no-op rebuild, rendering and decoration lifecycle', flush=True)


if __name__ == '__main__':
    main()
