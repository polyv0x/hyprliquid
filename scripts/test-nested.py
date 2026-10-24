#!/usr/bin/env python3
"""Exercise the plugin in a disposable nested Hyprland, using its own IPC socket."""
from pathlib import Path
import json
import os
import shutil
import subprocess
import tempfile
import time
import sys

ROOT = Path(__file__).resolve().parent.parent
FIXTURES = ROOT / 'build'
OUTPUT_DIRECTORY = os.environ.get('HYPRLIQUID_TEST_OUTPUT')
BUILD = Path(OUTPUT_DIRECTORY).resolve() if OUTPUT_DIRECTORY else FIXTURES
PLUGIN = Path(os.environ.get('HYPRLIQUID_TEST_PLUGIN', ROOT / 'hyprliquid.so')).resolve()
# Explicit output directories must be fresh: old screenshots must never inflate
# a before/after comparison. Compiled fixtures still come from the normal build.
BUILD.mkdir(parents=True, exist_ok=not OUTPUT_DIRECTORY)
sys.path.insert(0, str(ROOT / 'tests'))
from control_api import verify_control
from window_scene import verify_window_scene
from liquid_scene import verify_liquid
from hover_angle import verify_hover_angle


def wait_for(predicate, process, seconds=20):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        value = predicate()
        if value:
            return value
        if process.poll() is not None:
            raise RuntimeError(f'Nested Hyprland exited ({process.returncode}); see {BUILD / "nested.log"}')
        time.sleep(0.1)
    raise TimeoutError('Timed out waiting for nested Hyprland')


def main():
    # Unix socket paths are limited to 108 bytes: keep the runtime path short.
    with tempfile.TemporaryDirectory(prefix='hlq-', dir='/tmp') as temporary:
        runtime = Path(temporary)
        env = os.environ.copy()
        parent_runtime = Path(env.get('XDG_RUNTIME_DIR', '/run/user/1000'))
        parent_display = env.get('WAYLAND_DISPLAY', 'wayland-1')
        parent_socket = Path(parent_display) if parent_display.startswith('/') else parent_runtime / parent_display
        env.update(XDG_RUNTIME_DIR=str(runtime), XDG_CONFIG_HOME=str(BUILD / 'nested-config'),
                   XDG_CACHE_HOME=str(BUILD / 'nested-cache'), XDG_STATE_HOME=str(BUILD / 'nested-state'),
                   WAYLAND_DISPLAY=str(parent_socket), HYPRLAND_NO_SD_VARS='1', HYPRLAND_NO_SD_NOTIFY='1')
        env.pop('HYPRLAND_INSTANCE_SIGNATURE', None)
        # A nested compositor must not acquire the host's logind seat. A failed
        # DRM probe can otherwise leave Aquamarine with an inactive session.
        env['LIBSEAT_BACKEND'] = 'seatd'
        env['SEATD_SOCK'] = str(runtime / 'no-seatd.sock')
        clients = []
        config_path = runtime / 'hyprland.lua'
        shutil.copyfile(ROOT / 'tests/nested.lua', config_path)
        with (BUILD / 'nested.log').open('w') as log:
            compositor = subprocess.Popen(['Hyprland', '--config', str(config_path)], env=env, stdout=log, stderr=log)
            try:
                socket = wait_for(lambda: next(runtime.glob('hypr/*/.socket.sock'), None), compositor)
                env['HYPRLAND_INSTANCE_SIGNATURE'] = socket.parent.name
                display = wait_for(lambda: next((p for p in runtime.glob('wayland-*') if p.is_socket()), None), compositor)
                env['WAYLAND_DISPLAY'] = str(display)

                def ctl(*args):
                    result = subprocess.run(['hyprctl', '-i', socket.parent.name, *args], env=env, text=True, capture_output=True, timeout=10)
                    if result.returncode or (args[0] in {'dispatch', 'eval', 'plugin'} and result.stdout.strip() != 'ok'):
                        raise RuntimeError(f'hyprctl {args}: {result.stdout}{result.stderr}')
                    return result.stdout.strip()

                parent_outputs = wait_for(lambda: json.loads(ctl('-j', 'monitors')), compositor)
                # A parent tiling WM can resize the nested Wayland window after
                # we queried its geometry. Use a fixed virtual output for pixel
                # tests, retaining the Wayland backend only for GPU allocation.
                assert ctl('output', 'create', 'headless', 'HYPRLIQUID-TEST') == 'ok'
                for output in parent_outputs:
                    assert ctl('output', 'remove', output['name']) == 'ok'
                wait_for(lambda: len(json.loads(ctl('-j', 'monitors'))) == 1 and
                         json.loads(ctl('-j', 'monitors'))[0]['name'] == 'HYPRLIQUID-TEST', compositor)
                response = ctl('plugin', 'load', str(PLUGIN))
                if response != 'ok':
                    raise RuntimeError(response)
                assert any(p['name'] == 'hyprliquid' for p in json.loads(ctl('-j', 'plugin', 'list')))
                errors = ctl('configerrors')
                if errors:
                    raise RuntimeError(f'Config errors: {errors}')
                shadow = json.loads(ctl('-j', 'getoption', 'decoration:shadow:enabled'))
                assert shadow.get('bool', shadow.get('int')) == 1, shadow
                print('PASS: load and Lua configuration (including signed preset values)', flush=True)
                control = verify_control(ctl, config_path)
                security_plugin = FIXTURES / 'security-test.so'
                # The fault-injection fixture links the current checkout's library.
                if security_plugin.exists() and PLUGIN == (ROOT / 'hyprliquid.so').resolve():
                    ctl('plugin', 'load', str(security_plugin))
                    ctl('hyprliquid-test:security', 'run')
                    result = wait_for(lambda: (status if (status := ctl('hyprliquid-test:security', 'status')) != 'pending' else None), compositor)
                    assert result == 'pass', result
                    layer_binary = FIXTURES / 'layer-test'
                    if layer_binary.exists():
                        saved_config = config_path.read_text()
                        config_path.write_text(saved_config + '\nhl.plugin.hyprliquid.config({ layers = { enabled = true, mask_mode = "alpha" } })\n')
                        ctl('reload')
                        layer = subprocess.Popen([str(layer_binary)], env=env | {'GSK_RENDERER': 'cairo', 'GTK_A11Y': 'none'}, stdout=log, stderr=log)
                        clients.append(layer)
                        try:
                            wait_for(lambda: 'hyprliquid-vram-test' in ctl('-j', 'layers'), compositor)
                            try:
                                wait_for(lambda: any(item['cached'] for item in control('buffers')['buffers']['layers']), compositor)
                            except TimeoutError:
                                (BUILD / 'security-layer-failure.json').write_text(json.dumps({
                                    'buffers': control('buffers'), 'layers': json.loads(ctl('-j', 'layers')),
                                    'enabled': json.loads(ctl('-j', 'getoption', 'plugin:hyprliquid:layers:enabled')),
                                    'config_errors': ctl('configerrors'),
                                }, indent=2))
                                raise
                            for preset, model in [('frosted', 'legacy'), ('frosted-liquid', 'liquid'), ('frosted', 'legacy'), ('frosted-liquid-medium', 'liquid')]:
                                config_path.write_text(saved_config + '\nhl.plugin.hyprliquid.config({ layers = { enabled = true, mask_mode = "alpha" } })\n'
                                                       'hl.plugin.hyprliquid.layer("hyprliquid-vram-test", { preset = ' + json.dumps(preset) + ' })\n')
                                ctl('reload')
                                wait_for(lambda: any(item['cached'] and item['model'] == model
                                                     for item in control('buffers')['buffers']['layers']), compositor)
                                wait_for(lambda: any(b['role'] == 'layer-material-backdrop'
                                                     for b in control('buffers')['buffers']['buffers']) == (model == 'liquid'), compositor)
                                data = control('buffers')['buffers']
                                assert any(b['role'] == 'layer-material-backdrop' for b in data['buffers']) == (model == 'liquid')
                                if model == 'liquid':
                                    assert all(item['model_data']['shadow_owner'] == 'surface' for item in data['layers'])
                            ctl('hyprliquid-test:security', 'lock')
                            assert ctl('hyprliquid-test:security', 'locked') == 'yes'
                            time.sleep(0.2)
                            assert not control('buffers')['buffers']['layers'], 'Lock retained a desktop layer backdrop'
                            ctl('hyprliquid-test:security', 'unlock')
                            wait_for(lambda: any(item['cached'] for item in control('buffers')['buffers']['layers']), compositor)
                        finally:
                            ctl('hyprliquid-test:security', 'unlock')
                            layer.terminate()
                            layer.wait(timeout=5)
                            config_path.write_text(saved_config)
                            ctl('reload')
                        wait_for(lambda: not control('buffers')['buffers']['layers'], compositor)
                        print('PASS: layer caches are discarded while locked and rebuilt after unlock', flush=True)
                    ctl('plugin', 'unload', str(security_plugin))
                    print('PASS: invalid GPU sizes, failed allocation/blur, GL state restoration and byte overflow', flush=True)
                    # Fault injection deliberately increments failure diagnostics.
                    # Start the rendering suite with a fresh plugin instance.
                    ctl('plugin', 'unload', str(PLUGIN))
                    ctl('plugin', 'load', str(PLUGIN))
                elif '--security-only' in sys.argv:
                    raise RuntimeError('Build build/security-test.so before running --security-only')
                if '--security-only' in sys.argv:
                    return
                ctl('dismissnotify')

                if '--liquid-only' in sys.argv:
                    verify_liquid(ctl, env, log, clients, BUILD, lambda predicate: wait_for(predicate, compositor))
                    return

                if '--hover-only' in sys.argv:
                    verify_hover_angle(ctl, env, log, clients, BUILD, lambda predicate: wait_for(predicate, compositor), PLUGIN)
                    return

                if shutil.which('kitty') and shutil.which('grim'):
                    backdrop = "import time; print('\\x1b[2J\\x1b[H' + '\\n'.join(''.join('\\x1b[48;2;%d;%d;%dm  ' % (((x//6+y//4)%3==0)*150+20, ((x//6+y//4)%3==1)*140+20, ((x//6+y//4)%3==2)*160+30) for x in range(90)) for y in range(50)), flush=True); time.sleep(120)"
                    clients.append(subprocess.Popen(['kitty', '--start-as=normal', '--class', 'hyprliquid-background', '-o', 'background_opacity=1',
                                                     '-o', 'remember_window_size=no',
                                                     '-o', 'cursor_blink_interval=0', '-o', 'font_size=18',
                                                     'python3', '-c', backdrop], env=env, stdout=log, stderr=log))
                    background = wait_for(lambda: next((c for c in json.loads(ctl('-j', 'clients')) if c['class'] == 'hyprliquid-background'), None), compositor)

                    def window_action(address, action, values):
                        ctl('dispatch', 'hl.dsp.window.' + action + '({ window = ' + json.dumps('address:' + address) + ', ' + values + ' })')

                    monitor = json.loads(ctl('-j', 'monitors'))[0]
                    window_action(background['address'], 'fullscreen', 'mode = "maximized", action = "unset"')
                    window_action(background['address'], 'float', 'action = "enable"')
                    window_action(background['address'], 'resize', f'x = {monitor["width"]}, y = {monitor["height"]}, relative = false')
                    window_action(background['address'], 'move', 'x = 0, y = 0, relative = false')
                    command = ['kitty', '--start-as=normal', '--class', 'hyprliquid-test', '--title', 'Hyprliquid shader test',
                               '-o', 'remember_window_size=no',
                               '-o', 'background_opacity=0.2', '-o', 'cursor_blink_interval=0', '-o', 'font_size=18',
                               'sh', '-c', 'printf "Hyprliquid native shader\\n\\nOpaque text over a transparent window.\\n"; sleep 120']
                    clients.append(subprocess.Popen(command, env=env, stdout=log, stderr=log))
                    windows = wait_for(lambda: [c for c in json.loads(ctl('-j', 'clients')) if c['class'] == 'hyprliquid-test'], compositor)
                    address = windows[0]['address']
                    window_action(address, 'fullscreen', 'mode = "maximized", action = "unset"')
                    window_action(address, 'float', 'action = "enable"')
                    monitor = json.loads(ctl('-j', 'monitors'))[0]
                    width, height = min(700, monitor['width'] - 100), min(420, monitor['height'] - 100)
                    x, y = (monitor['width'] - width) // 2, (monitor['height'] - height) // 2
                    position = f'x = {x}, y = {y}, relative = false'
                    shifted = f'x = {x + 1}, y = {y}, relative = false'
                    window_action(address, 'resize', f'x = {width}, y = {height}, relative = false')
                    window_action(address, 'move', position)
                    window_action(address, 'alter_zorder', 'mode = "top"')
                    time.sleep(1)
                    geometry = next(c for c in json.loads(ctl('-j', 'clients')) if c['address'] == address)
                    assert geometry['floating'] and geometry['fullscreen'] == 0 and geometry['size'] == [width, height] and geometry['at'] == [x, y], geometry
                    (BUILD / 'nested-clients.json').write_text(ctl('-j', 'clients'))
                    # No move/resize/input between these captures: an accepted edit
                    # must repaint an otherwise idle transparent window itself.
                    def capture(name):
                        time.sleep(0.3)
                        path = BUILD / (name + '.png')
                        subprocess.run(['grim', str(path)], env=env, check=True, capture_output=True, timeout=10)
                        from PIL import Image
                        return Image.open(path).crop((x + 40, y + 40, x + width - 40, y + height - 40)).tobytes()

                    original = capture('nested-control-baseline')
                    assert control('apply', patch={'overrides': {'blur_sigma': 0, 'face_opacity': 0, 'rim_opacity': 0}})['ok']
                    changed = capture('nested-control-edit')
                    assert changed != original, 'Runtime edit did not repaint idle window'
                    assert control('reset')['ok']
                    assert capture('nested-control-reset') == original, 'Runtime reset changed configured custom preset'
                    assert control('apply', patch={'enabled': False})['ok']
                    assert capture('nested-control-disabled') != original, 'Runtime disable did not repaint idle window'
                    assert control('reset')['ok']
                    ctl('eval', 'hl.window_rule({ name = "runtime-tagged", match = { class = "^hyprliquid-test$" }, tag = "+hyprliquid_preset_test" })')
                    tagged = capture('nested-control-tagged')
                    assert control('apply', profile={'base_preset': 'lens', 'theme': 'light', 'overrides': {'blur_sigma': 0, 'glass_opacity': 0}})['ok']
                    assert capture('nested-control-tagged-edit') == tagged, 'Runtime edit leaked into an explicitly tagged preset'
                    assert control('reset')['ok']
                    ctl('reload')
                    # Recreate the foreground after testing dynamic tags: a
                    # native config reload can retain tags/stacking on the old
                    # floating fixture, hiding the pane behind its backdrop.
                    clients[-1].terminate()
                    clients[-1].wait(timeout=5)
                    wait_for(lambda: not any(c['class'] == 'hyprliquid-test' for c in json.loads(ctl('-j', 'clients'))), compositor)
                    clients.append(subprocess.Popen(command, env=env, stdout=log, stderr=log))
                    fresh = wait_for(lambda: next((c for c in json.loads(ctl('-j', 'clients')) if c['class'] == 'hyprliquid-test'), None), compositor)
                    address = fresh['address']
                    window_action(address, 'float', 'action = "enable"')
                    window_action(address, 'resize', f'x = {width}, y = {height}, relative = false')
                    window_action(address, 'move', position)
                    print('PASS: idle live edit/reset and per-window preset isolation', flush=True)
                    public_presets = {'frosted', 'soft-glass', 'clear-glass', 'frosted-glass', 'frosted-glass-clear-edge', 'frosted-liquid', 'frosted-liquid-medium'}
                    listed = {p['id'] for p in control('describe')['presets']}
                    assert public_presets <= listed, listed
                    assert not listed & {'default', 'measured', 'platform', 'lens', 'lighter', 'darker', 'ultra_darker', 'no_bleed', 'liquid-medium', 'clear-edge', 'frosted-clear-edge'}, listed
                    # Renaming must preserve existing profiles, including the
                    # saved lens-based frost settings that became built-ins.
                    equivalents = [
                        ('measured', 'frosted', {}),
                        ('platform', 'soft-glass', {}),
                        ('lens', 'clear-glass', {}),
                        ('lens', 'frosted-glass', {'blur_alpha0': 0.96, 'blur_alpha3': 1}),
                        ('lens', 'frosted-glass-clear-edge', {'blur_alpha0': 0.02, 'blur_alpha3': 1, 'blur_dist3': 38.4}),
                        ('clear-edge', 'frosted-glass-clear-edge', {}),
                        ('frosted-clear-edge', 'frosted-glass-clear-edge', {}),
                        ('liquid-medium', 'frosted-liquid', {}),
                    ]
                    for legacy, preset, overrides in equivalents:
                        before = control('apply', profile={'base_preset': legacy, 'overrides': overrides})
                        assert before['ok'], before
                        pixels = capture('nested-legacy-' + preset)
                        after = control('apply', profile={'base_preset': preset})
                        assert after['ok'] and after['state']['base_preset'] == preset, after
                        assert before['state']['material'] == after['state']['material'], preset
                        assert capture('nested-renamed-' + preset) == pixels, preset
                    assert control('reset')['ok']
                    print('PASS: renamed presets and saved frosted profiles render identically', flush=True)
                    for preset in sorted(public_presets):
                        assert control('apply', profile={'base_preset': preset, 'theme': 'dark'})['ok']
                        dark_pixels = capture('nested-theme-' + preset + '-dark')
                        assert control('apply', patch={'theme': 'light'})['ok']
                        assert capture('nested-theme-' + preset + '-light') != dark_pixels, preset + ' theme switch did not repaint'
                        assert control('apply', patch={'theme': 'dark'})['ok']
                        assert capture('nested-theme-' + preset + '-dark-restored') == dark_pixels, preset + ' theme round trip changed pixels'
                    assert control('reset')['ok']
                    print('PASS: all seven dark/light variants repaint idle windows and restore identical pixels', flush=True)
                    for preset in ['frosted', 'soft-glass', 'frosted-glass', 'frosted-glass-clear-edge', 'clear-glass']:
                        ctl('eval', 'hl.plugin.hyprliquid.config({ default_preset = ' + json.dumps(preset) + ' })')
                        window_action(address, 'alter_zorder', 'mode = "top"')
                        option = json.loads(ctl('-j', 'getoption', 'plugin:hyprliquid:default_preset'))
                        assert option.get('str') == preset, option
                        window_action(address, 'move', shifted)
                        window_action(address, 'move', position)
                        time.sleep(0.25)
                        subprocess.run(['grim', str(BUILD / f'nested-{preset}.png')], env=env, check=True, capture_output=True, timeout=10)
                    ctl('eval', 'hl.plugin.hyprliquid.config({ enabled = false })')
                    assert json.loads(ctl('-j', 'getoption', 'plugin:hyprliquid:enabled')).get('int') == 0
                    window_action(address, 'move', shifted)
                    window_action(address, 'move', position)
                    time.sleep(0.25)
                    subprocess.run(['grim', str(BUILD / 'nested-disabled.png')], env=env, check=True, capture_output=True, timeout=10)
                    ctl('eval', 'hl.plugin.hyprliquid.config({ enabled = true })')
                    window_action(address, 'fullscreen', 'mode = "fullscreen", action = "set"')
                    time.sleep(0.25)
                    assert next(c for c in json.loads(ctl('-j', 'clients')) if c['address'] == address)['fullscreen'] != 0
                    window_action(address, 'fullscreen', 'mode = "fullscreen", action = "unset"')
                    window_action(address, 'move', 'x = -40, y = -40, relative = false')
                    time.sleep(0.25)
                    edge = next(c for c in json.loads(ctl('-j', 'clients')) if c['address'] == address)
                    assert edge['fullscreen'] == 0 and edge['at'] == [-40, -40], edge
                    subprocess.run(['grim', str(BUILD / 'nested-edge.png')], env=env, check=True, capture_output=True, timeout=10)
                    assert (BUILD / 'nested-frosted.png').read_bytes() != (BUILD / 'nested-soft-glass.png').read_bytes(), 'Preset change did not render'
                    assert (BUILD / 'nested-frosted-glass.png').read_bytes() != (BUILD / 'nested-frosted-glass-clear-edge.png').read_bytes(), 'Frosted edge treatments did not differ'
                    assert (BUILD / 'nested-clear-glass.png').read_bytes() != (BUILD / 'nested-disabled.png').read_bytes(), 'Disable did not render'
                    print('PASS: render, switch presets, disable/enable, fullscreen, offscreen padding', flush=True)
                    verify_window_scene(ctl, env, log, clients, BUILD,
                                        lambda predicate: wait_for(predicate, compositor))
                    verify_liquid(ctl, env, log, clients, BUILD,
                                  lambda predicate: wait_for(predicate, compositor))
                    verify_hover_angle(ctl, env, log, clients, BUILD,
                                       lambda predicate: wait_for(predicate, compositor), PLUGIN)
                else:
                    print('SKIP: window/screenshot exercise requires kitty and grim', flush=True)

                epoch = control('state')['state']['epoch']
                for _ in range(2):
                    assert ctl('plugin', 'unload', str(PLUGIN)) == 'ok'
                    assert ctl('plugin', 'load', str(PLUGIN)) == 'ok'
                    current = control('state')['state']
                    assert current['epoch'] != epoch and not current['active']
                    epoch = current['epoch']
                print('PASS: two unload/reload cycles', flush=True)
                (BUILD / 'nested-compositor.log').write_text((socket.parent / 'hyprland.log').read_text())
                ctl('dispatch', 'hl.dsp.exit()')
                compositor.wait(timeout=10)
            finally:
                logs = list(runtime.glob('hypr/*/hyprland.log'))
                if logs:
                    (BUILD / 'nested-compositor.log').write_text(logs[-1].read_text())
                for client in clients:
                    if client.poll() is None:
                        client.terminate()
                if compositor.poll() is None:
                    compositor.terminate()
                    try:
                        compositor.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        compositor.kill()
                        compositor.wait()


if __name__ == '__main__':
    main()
