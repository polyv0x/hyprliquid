#!/usr/bin/env python3
"""Capture real native glass windows over a rendered primitive scene.

All generated images stay in ignored directories. A separate compositor renders
the 1920x1200 logical scene at twice the pixel density, then grim resolves it to
1920x1200 for antialiased borders. The desktop's renderer/settings are untouched.
"""
from pathlib import Path
import argparse
import json
import os
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / 'build' / 'demo'
OUTPUT = ROOT / 'screenshots'


def wait_for(predicate, process, seconds=25):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        value = predicate()
        if value:
            return value
        if process.poll() is not None:
            raise RuntimeError('Demo process exited; see build/demo/session.log')
        time.sleep(0.1)
    raise TimeoutError('Demo did not become ready')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--supersample', type=int, choices=(1, 2, 3, 4), default=2,
                        help='Render density before resolving to 1920x1200 (default: 2; 1 disables supersampling)')
    parser.add_argument('--output', type=Path, default=OUTPUT, help='Directory for the eight PNG captures')
    args = parser.parse_args()
    output = args.output.resolve()
    BUILD.mkdir(parents=True, exist_ok=True)
    output.mkdir(parents=True, exist_ok=True)
    for variant, name in enumerate(['classic', 'primitives', 'dark']):
        path = BUILD / (name + '.ppm')
        inputs = [ROOT / 'demo/backdrop.frag', ROOT / 'build/demo-scene']
        if not path.exists() or path.stat().st_mtime < max(p.stat().st_mtime for p in inputs):
            subprocess.run([str(inputs[1]), str(inputs[0]), str(path), '1920', '1200', str(variant)],
                           env={**os.environ, 'LIBGL_ALWAYS_SOFTWARE': '1'}, check=True)

    with tempfile.TemporaryDirectory(prefix='hlqd-', dir='/tmp') as directory, (BUILD / 'session.log').open('w') as log:
        runtime = Path(directory)
        env = os.environ.copy()
        display = Path(env.get('WAYLAND_DISPLAY', 'wayland-1'))
        if not display.is_absolute():
            display = Path(env.get('XDG_RUNTIME_DIR', '/run/user/1000')) / display
        env.update(XDG_RUNTIME_DIR=str(runtime), WAYLAND_DISPLAY=str(display),
                   XDG_CONFIG_HOME=str(BUILD / 'config'), XDG_CACHE_HOME=str(BUILD / 'cache'),
                   XDG_STATE_HOME=str(BUILD / 'state'), HYPRLAND_NO_SD_VARS='1', HYPRLAND_NO_SD_NOTIFY='1',
                   LIBSEAT_BACKEND='seatd', SEATD_SOCK=str(runtime / 'no-seatd.sock'),
                   QT_QPA_PLATFORM='wayland', QT_WAYLAND_DISABLE_WINDOWDECORATION='1')
        env.pop('HYPRLAND_INSTANCE_SIGNATURE', None)
        clients = []
        compositor = subprocess.Popen(['Hyprland', '--config', str(ROOT / 'demo/hyprland.lua')],
                                      env=env, stdout=log, stderr=log)
        try:
            socket = wait_for(lambda: next(runtime.glob('hypr/*/.socket.sock'), None), compositor)
            env['HYPRLAND_INSTANCE_SIGNATURE'] = socket.parent.name
            env['WAYLAND_DISPLAY'] = str(wait_for(lambda: next((p for p in runtime.glob('wayland-*') if p.is_socket()), None), compositor))

            def ctl(*args):
                response = subprocess.run(['hyprctl', '-i', socket.parent.name, *args], env=env,
                                          text=True, capture_output=True, timeout=15)
                if response.returncode or (args[0] in {'dispatch', 'eval', 'plugin'} and response.stdout.strip() != 'ok'):
                    raise RuntimeError(f'hyprctl {args}: {response.stdout}{response.stderr}')
                return response.stdout.strip()

            wait_for(lambda: json.loads(ctl('-j', 'monitors')), compositor)
            ctl('output', 'create', 'headless', 'HLQ-DEMO')
            # Loading the plugin reloads the Lua config, including its native
            # output mode. Set capture density only after that reload completes.
            ctl('plugin', 'load', str(ROOT / 'hyprliquid.so'))
            density = args.supersample
            dimensions = [1920 * density, 1200 * density, density]
            ctl('eval', 'hl.monitor({ output = "HLQ-DEMO", mode = "' +
                f'{dimensions[0]}x{dimensions[1]}@60", position = "0x0", scale = {density}' + ' })')
            monitor = wait_for(lambda: next((m for m in json.loads(ctl('-j', 'monitors'))
                if m['name'] == 'HLQ-DEMO' and [m['width'], m['height'], m['scale']] == dimensions), None), compositor)
            assert [monitor['width'] / density, monitor['height'] / density] == [1920, 1200], monitor
            errors = ctl('configerrors')
            if errors:
                raise RuntimeError(errors)
            ctl('dismissnotify')

            def action(address, method, arguments):
                return ctl('dispatch', 'hl.dsp.window.' + method + '({ window = ' + json.dumps('address:' + address) + ', ' + arguments + ' })')

            def launch(role, x, y, width, height, *extra):
                process = subprocess.Popen([sys.executable, str(ROOT / 'demo/window.py'), role,
                                            '--width', str(width), '--height', str(height), *extra],
                                           env=env, stdout=log, stderr=log)
                clients.append(process)
                window = wait_for(lambda: next((w for w in json.loads(ctl('-j', 'clients'))
                                                if w['pid'] == process.pid and w['mapped']), None), process)
                address = window['address']
                action(address, 'move', 'monitor = "HLQ-DEMO", follow = false')
                action(address, 'float', 'action = "enable"')
                # Wayland configure/ack is asynchronous, especially when a
                # client moves from the nested output to the capture output.
                # Wait for exact, settled geometry before taking the shot.
                for attempt in range(4):
                    action(address, 'resize', f'x = {width}, y = {height}, relative = false')
                    action(address, 'move', f'x = {x}, y = {y}, relative = false')
                    time.sleep(0.25)
                    current = next(w for w in json.loads(ctl('-j', 'clients')) if w['address'] == address)
                    if current['at'] == [x, y] and current['size'] == [width, height]:
                        break
                else:
                    raise RuntimeError(f'Unexpected demo window geometry: {current}')
                action(address, 'alter_zorder', 'mode = "top"')
                return process

            shots = [
                ('01-soft-glass', 'primitives', [('soft-glass', 510, 365, 900, 540)], 'SOFT GLASS / 01'),
                ('02-frosted', 'primitives', [('frosted', 510, 365, 900, 540)], 'FROSTED / 02'),
                ('03-clear-glass', 'primitives', [('clear-glass', 510, 365, 900, 540)], 'CLEAR GLASS / 03'),
                ('04-classic', 'classic', [('soft-glass', 510, 365, 900, 540)], 'ROUNDED PRIMITIVES / 04'),
                ('05-materials', 'primitives', [('frosted', 105, 345, 530, 600), ('soft-glass', 695, 345, 530, 600),
                                               ('clear-glass', 1285, 345, 530, 600)], 'THREE MATERIALS / 05'),
                ('06-frosted-glass', 'dark', [('frosted-glass', 510, 365, 900, 540)], 'FROSTED GLASS / 06'),
                ('07-frosted-glass-clear-edge', 'dark', [('frosted-glass-clear-edge', 510, 365, 900, 540)], 'FROSTED GLASS CLEAR EDGE / 07'),
                ('08-frosted-materials', 'dark', [('soft-glass', 105, 345, 530, 600), ('frosted-glass', 695, 345, 530, 600),
                                              ('frosted-glass-clear-edge', 1285, 345, 530, 600)], 'FROSTED MATERIALS / 08'),
            ]
            for filename, scene, panes, label in shots:
                theme = ['--dark'] if scene == 'dark' else []
                scene_clients = [launch('backdrop', 0, 0, 1920, 1200, '--image', str(BUILD / (scene + '.ppm')),
                                        '--label', label, *theme)]
                for role, x, y, width, height in panes:
                    scene_clients.append(launch(role, x, y, width, height, *theme,
                                                *(['--compact'] if len(panes) > 1 else [])))
                # Park the nested cursor on its other output, outside the shot.
                ctl('dispatch', 'hl.dsp.cursor.move({ x = 2200, y = 300 })')
                time.sleep(1)
                monitor = next(m for m in json.loads(ctl('-j', 'monitors')) if m['name'] == 'HLQ-DEMO')
                assert [monitor['width'], monitor['height'], monitor['scale']] == dimensions, monitor
                path = output / (filename + '.png')
                # Render the same logical geometry/materials at higher density
                # and resolve during capture; no edits to the production shader.
                subprocess.run(['grim', '-s', '1', '-o', 'HLQ-DEMO', str(path)], env=env, check=True, capture_output=True, timeout=15)
                print('Captured ' + str(path), flush=True)
                for client in reversed(scene_clients):
                    client.terminate()
                    client.wait(timeout=10)
                wait_for(lambda: not json.loads(ctl('-j', 'clients')), compositor)
            (BUILD / 'plugins.json').write_text(ctl('-j', 'plugin', 'list'))
            (output / 'capture-settings.json').write_text(json.dumps({
                'supersample': density, 'render_size': dimensions[:2], 'image_size': [1920, 1200],
            }, indent=2) + '\n')
            assert not ctl('configerrors')
            ctl('dispatch', 'hl.dsp.exit()')
            compositor.wait(timeout=10)
        finally:
            for client in clients:
                if client.poll() is None:
                    client.terminate()
                    try:
                        client.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        client.kill()
                        client.wait()
            if compositor.poll() is None:
                compositor.terminate()
                try:
                    compositor.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    compositor.kill()
                    compositor.wait()
            for path in runtime.glob('hypr/*/hyprland.log'):
                (BUILD / 'compositor.log').write_text(path.read_text())


if __name__ == '__main__':
    main()
