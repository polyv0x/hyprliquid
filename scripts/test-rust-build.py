#!/usr/bin/env python3
"""Check clean builds and schema/bridge invalidation in an isolated source copy.

Requires the normal plugin build dependencies. Never edits the working checkout
or loads a plugin. Pass --quick to skip the initial full plugin/render build.
"""
from pathlib import Path
import argparse
import json
import os
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--quick', action='store_true')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='hyprliquid-rust-build-') as directory:
        root = Path(directory)
        for name in ('src', 'crates', 'scripts', 'materials', 'shaders', 'tests', '.cargo'):
            shutil.copytree(ROOT / name, root / name, ignore=shutil.ignore_patterns('__pycache__'))
        for name in ('Makefile', 'Cargo.toml', 'Cargo.lock', 'rust-toolchain.toml'):
            shutil.copy2(ROOT / name, root / name)
        env = os.environ.copy()
        env.pop('CARGO_TARGET_DIR', None)

        def run(*command):
            subprocess.run(command, cwd=root, env=env, check=True)

        if not args.quick:
            run('make', '-j4', 'all', 'test', 'build/security-test.so', 'build/capture-test.so')
            symbols = subprocess.check_output(['nm', '-D', '--defined-only', 'hyprliquid.so'], cwd=root, text=True)
            assert 'cxxbridge' not in symbols and '___rust' not in symbols, 'Rust runtime escaped plugin visibility'
            capture_symbols = subprocess.check_output(['nm', '-D', '--defined-only', 'build/capture-test.so'], cwd=root, text=True)
            assert {line.split()[-1] for line in capture_symbols.splitlines()} == {'pluginAPIVersion', 'pluginInit', 'pluginExit'}, 'Capture fixture can interpose on comparison plugins'
            print('PASS: clean mixed Rust/C++ plugin and existing tests', flush=True)
        run('make', '-j4', 'build/RustCore.o')

        probe = root / 'build/probe.cpp'
        probe.write_text('''
#include "Material.hpp"
#include "hyprliquid-bridge/ffi.rs.h"
#include <cassert>
int main() {
    auto m = SMaterial{};
    m.blur_sigma = 0.0f / 0.0f;
    sanitizeMaterial(m);
    assert(m.blur_sigma == EXPECTED_SCALAR);
    const auto liquid = liquidControlDefaults(true);
    assert(liquid.blur_sigma == EXPECTED_RADIUS);
    assert(effectiveLiquidMaterial(liquid).blur.radius == LIQUID_DARK.blur.radius);
#ifdef CHECK_BRIDGE
    static_assert(static_cast<int>(hyprliquid::ffi::Status::BuildDependencyProbe) == 3);
#endif
}
''')

        def check(scalar, radius, bridge=False):
            run('make', '-j4', 'build/RustCore.o')
            command = ['c++', '-std=c++23', '-Isrc', '-Ibuild', '-Ibuild/cargo/cxxbridge',
                       f'-DEXPECTED_SCALAR={scalar}f', f'-DEXPECTED_RADIUS={radius}f']
            if bridge:
                command.append('-DCHECK_BRIDGE')
            run(*command, 'build/probe.cpp', 'build/RustCore.o',
                'build/cargo/release/libhyprliquid_bridge.a', '-ldl', '-lpthread', '-lm', '-o', 'build/probe')
            run('./build/probe')

        check('19.80', '6.0')
        params = root / 'src/MaterialParams.inc'
        params.write_text(params.read_text().replace('19.80f', '19.81f'))
        check('19.81', '6.0')
        print('PASS: scalar descriptor changes rebuild Rust and C++ consumers', flush=True)

        data = root / 'materials/liquid.json'
        value = json.loads(data.read_text())
        value['variants']['dark']['blur']['radius'] = 7.0
        data.write_text(json.dumps(value))
        check('19.81', '7.0')
        print('PASS: material data changes rebuild both generated representations', flush=True)

        bridge = root / 'crates/hyprliquid-bridge/src/ffi.rs.in'
        bridge.write_text(bridge.read_text().replace('Ok, InvalidGeometry, InternalFailure',
                                                    'Ok, InvalidGeometry, InternalFailure, BuildDependencyProbe'))
        check('19.81', '7.0', bridge=True)
        print('PASS: bridge declaration changes regenerate and recompile its C++ header', flush=True)


if __name__ == '__main__':
    main()
