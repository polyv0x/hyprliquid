#!/usr/bin/env python3
"""Build the optional native workspace-capture client from installed protocols."""
import os
from pathlib import Path
import shlex
import subprocess
import sys


def pkg(*arguments):
    return subprocess.check_output(['pkg-config', *arguments], text=True).strip()


output = Path(sys.argv[1]).resolve()
generated = output.parent / 'workspace-capture-protocols'
generated.mkdir(parents=True, exist_ok=True)
standard = Path(pkg('--variable=pkgdatadir', 'wayland-protocols'))
hyprland = Path(pkg('--variable=pkgdatadir', 'hyprland-protocols'))
protocols = [standard / 'staging' / name / (name + '-v1.xml') for name in (
    'ext-workspace', 'ext-image-copy-capture', 'ext-image-capture-source', 'ext-foreign-toplevel-list')]
protocols.append(hyprland / 'protocols/hyprland-workspace-image-capture-source-v1.xml')
objects = []
for protocol in protocols:
    source = generated / (protocol.stem + '.c')
    header = generated / (protocol.stem + '.h')
    obj = source.with_suffix('.o')
    subprocess.run(['wayland-scanner', 'client-header', str(protocol), str(header)], check=True)
    subprocess.run(['wayland-scanner', 'private-code', str(protocol), str(source)], check=True)
    subprocess.run([*shlex.split(os.environ.get('CC', 'cc')), '-c', str(source), '-o', str(obj),
                    *shlex.split(pkg('--cflags', 'wayland-client'))], check=True)
    objects.append(str(obj))
source = Path(__file__).resolve().parent.parent / 'tests/workspace-capture.cpp'
subprocess.run([*shlex.split(os.environ.get('CXX', 'c++')),
                *shlex.split(os.environ.get('CXXFLAGS', '-O2 -std=c++23')),
                '-I' + str(generated), str(source), *objects, '-o', str(output),
                *shlex.split(pkg('--cflags', '--libs', 'wayland-client'))], check=True)
