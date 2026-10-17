#!/usr/bin/env python3
"""Capture real native glass windows over a rendered primitive scene.

All generated images stay in ignored directories. A separate compositor and a
1920x1200 virtual output keep captures independent of the user's desktop layout.
"""
from pathlib import Path
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
    BUILD.mkdir(parents=True, exist_ok=True)
    OUTPUT.mkdir(exist_ok=True)
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
            monitor = wait_for(lambda: next((m for m in json.loads(ctl('-j', 'monitors')) if m['name'] == 'HLQ-DEMO'), None), compositor)
            assert [monitor['width'], monitor['height'], monitor['scale']] == [1920, 1200, 1], monitor
            ctl('plugin', 'load', str(ROOT / 'hyprliquid.so'))
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
                ('01-platform', 'primitives', [('platform', 510, 365, 900, 540)], 'PLATFORM / 01'),
                ('02-measured', 'primitives', [('measured', 510, 365, 900, 540)], 'MEASURED / 02'),
                ('03-lens', 'primitives', [('lens', 510, 365, 900, 540)], 'LENS / 03'),
                ('04-classic', 'classic', [('platform', 510, 365, 900, 540)], 'ROUNDED PRIMITIVES / 04'),
                ('05-materials', 'primitives', [('measured', 105, 345, 530, 600), ('platform', 695, 345, 530, 600),
                                               ('lens', 1285, 345, 530, 600)], 'THREE MATERIALS / 05'),
                ('06-darker', 'dark', [('darker', 510, 365, 900, 540)], 'DARKER / 06'),
                ('07-ultra-dark', 'dark', [('ultra_darker', 510, 365, 900, 540)], 'ULTRA DARK / 07'),
                ('08-dark-materials', 'dark', [('platform', 105, 345, 530, 600), ('darker', 695, 345, 530, 600),
                                              ('ultra_darker', 1285, 345, 530, 600)], 'DARK MATERIALS / 08'),
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
                path = OUTPUT / (filename + '.png')
                subprocess.run(['grim', '-o', 'HLQ-DEMO', str(path)], env=env, check=True, capture_output=True, timeout=15)
                print('Captured ' + str(path), flush=True)
                for client in reversed(scene_clients):
                    client.terminate()
                    client.wait(timeout=10)
                wait_for(lambda: not json.loads(ctl('-j', 'clients')), compositor)
            (BUILD / 'plugins.json').write_text(ctl('-j', 'plugin', 'list'))
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
