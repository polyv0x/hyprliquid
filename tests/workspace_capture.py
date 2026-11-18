"""Capture an isolated workspace with protected content beneath warmed window and layer glass."""
import hashlib
import json
from pathlib import Path
import subprocess
import time

from PIL import Image, ImageChops


def verify_workspace_capture(ctl, env, log, clients, output, wait, fixtures, config_path, control):
    capture_binary = fixtures / 'workspace-capture-test'
    assert capture_binary.exists(), 'Build build/workspace-capture-test first (Hyprland 0.57+)'
    capture_plugin = fixtures / 'capture-test.so'
    assert capture_plugin.exists(), 'Build build/capture-test.so for the unmasked desktop reference'
    saved_config = config_path.read_text()
    records = {}

    def action(window, name, arguments):
        ctl('dispatch', 'hl.dsp.window.' + name + '({ window = ' +
            json.dumps('address:' + window['address']) + ', ' + arguments + ' })')

    def launch(name, color, position, size):
        path = output / (name + '.color')
        path.write_text(color)
        script = r'''
from pathlib import Path
import sys, time
path = Path(sys.argv[1])
previous = None
print('\x1b[?25l', end='', flush=True)
while True:
    color = path.read_text().strip()
    if color != previous:
        print('\x1b]11;' + color + '\x07\x1b[2J\x1b[H', end='', flush=True)
        Path(str(path) + '.ack').write_text(color)
        previous = color
    time.sleep(.02)
'''
        process = subprocess.Popen(['kitty', '--class', name, '-o', 'remember_window_size=no',
            '-o', 'background_opacity=1', '-o', 'background=' + color, '-o', 'cursor_blink_interval=0',
            'python3', '-c', script, str(path)], env=env, stdout=log, stderr=log)
        clients.append(process)
        window = wait(lambda: next((w for w in json.loads(ctl('-j', 'clients')) if w['class'] == name), None))
        action(window, 'float', 'action = "enable"')
        action(window, 'resize', f'x = {size[0]}, y = {size[1]}, relative = false')
        action(window, 'move', f'x = {position[0]}, y = {position[1]}, relative = false')
        return window, path

    def image(path):
        with Image.open(path) as source:
            return source.convert('RGB')

    def desktop(name):
        time.sleep(.3)
        # Output screencopy (including grim) blacks out protected windows. Read
        # the real monitor framebuffer inside our disposable compositor instead.
        path = output / name
        ctl('hyprliquid-test:capture', str(path))
        wait(lambda: path.with_suffix('.json').exists())
        metadata = json.loads(path.with_suffix('.json').read_text())
        assert metadata['gl_error'] == 0, metadata
        assert metadata['gl_type'] == 5121 and metadata['pixel_bytes'] == 4, metadata
        order = {6408: 'RGBA', 32993: 'BGRA'}[metadata['gl_format']]
        raw = path.with_suffix('.raw').read_bytes()
        # Monitor framebuffer rows are already in logical top-to-bottom order
        # for this normal-transform output (window snapshots are different).
        pixels = Image.frombytes('RGBA', (metadata['width'], metadata['height']), raw, 'raw', order).convert('RGB')
        pixels.save(path.with_suffix('.png'))
        records[name] = {'metadata': metadata, 'sha256': hashlib.sha256(raw).hexdigest()}
        return pixels

    def workspace(name):
        workspace_name = json.loads(ctl('-j', 'activeworkspace'))['name']
        path = output / (name + '.ppm')
        result = subprocess.run([str(capture_binary), workspace_name, str(path)],
            env=env, capture_output=True, text=True, timeout=15)
        assert result.returncode == 0, (result.returncode, result.stdout, result.stderr)
        pixels = image(path)
        pixels.save(path.with_suffix('.png'))
        records[name] = {'metadata': json.loads(result.stdout),
                         'sha256': hashlib.sha256(pixels.tobytes()).hexdigest()}
        # Positive controls: both the ordinary green window and opaque layer
        # markers must survive; an empty/missing-layer capture cannot pass.
        red, green, blue = pixels.getpixel((900, 480))
        assert green > 2 * red and green > 2 * blue, (name, 'control window missing', (red, green, blue))
        assert min(pixels.getpixel((140, 124))) > 180, (name, 'layer marker missing')
        red, green, blue = pixels.getpixel((300, 300))
        assert blue > red and blue > green, (name, 'glass receiver surface missing', (red, green, blue))
        return pixels

    def layer_config(preset, enabled=True):
        config_path.write_text(saved_config + '\nhl.plugin.hyprliquid.config({ layers = { enabled = ' +
            ('true' if enabled else 'false') + ', mask_mode = "alpha" } })\n' +
            'hl.plugin.hyprliquid.layer("hyprliquid-vram-test", { preset = ' + json.dumps(preset) + ' })\n' +
            'hl.config({ general = { border_size = 0 }, decoration = { rounding = 0, active_opacity = 1, '
            'inactive_opacity = 1, dim_inactive = false }, cursor = { inactive_timeout = 0.1, no_warps = true } })\n' +
            'hl.window_rule({ name = "capture-secret", match = { class = "^hyprliquid-capture-secret$" }, no_screen_share = true })\n')
        ctl('reload')
        assert not ctl('configerrors')
        # Reload restores the file's default preset and enabled flag. Apply the
        # tested window material afterwards so receiver caches match this arm.
        control('apply', profile={'enabled': True, 'base_preset': 'frosted-glass' if preset == 'frosted' else preset,
                                  'theme': 'dark', 'animate_display_angle': False, 'animate_hover_angle': False})

    ctl('plugin', 'load', str(capture_plugin))
    try:
        control('apply', profile={'enabled': True, 'base_preset': 'frosted-glass', 'theme': 'dark',
                                  'animate_display_angle': False, 'animate_hover_angle': False})
        layer_config('frosted')
        _, color_path = launch('hyprliquid-capture-secret', '#d02020', (80, 70), (620, 360))
        launch('hyprliquid-capture-control', '#20c040', (840, 420), (240, 200))
        receiver_state = output / 'capture-receiver.json'
        receiver_state.write_text(json.dumps({'root': {'alpha': 80}, 'child': {'alpha': 80}}))
        receiver_process = subprocess.Popen([str(fixtures / 'subsurface-test'), 'hyprliquid-glass-receiver', str(receiver_state)],
            env=env, stdout=log, stderr=log)
        clients.append(receiver_process)
        receiver = wait(lambda: next((w for w in json.loads(ctl('-j', 'clients')) if w['class'] == 'hyprliquid-glass-receiver'), None))
        action(receiver, 'float', 'action = "enable"')
        action(receiver, 'resize', 'x = 400, y = 260, relative = false')
        action(receiver, 'move', 'x = 250, y = 250, relative = false')
        receiver_owner = 'window:' + receiver['address']

        def window_counters():
            return next(item for item in control('buffers')['buffers']['windows'] if item['owner'] == receiver_owner)

        layer = subprocess.Popen([str(fixtures / 'layer-test')],
            env=env | {'GSK_RENDERER': 'cairo', 'GTK_A11Y': 'none'}, stdout=log, stderr=log)
        clients.append(layer)
        wait(lambda: 'hyprliquid-vram-test' in ctl('-j', 'layers'))
        ctl('dismissnotify')
        for preset in ('frosted', 'frosted-liquid-medium'):
            control('apply', profile={'enabled': True, 'base_preset': 'frosted-glass' if preset == 'frosted' else preset,
                                      'theme': 'dark', 'animate_display_angle': False, 'animate_hover_angle': False})
            layer_config(preset)
            wait(lambda: any(item['cached'] for item in control('buffers')['buffers']['layers']))
            desktop(preset + '-window-warm-initial')
            warm_before = window_counters()
            desktop(preset + '-window-warm-reused')
            warm_after = window_counters()
            assert warm_after['enabled'] and warm_after['captures'] > 0, warm_after
            assert warm_after['window_backdrop_reuses'] > warm_before['window_backdrop_reuses'], (warm_before, warm_after)
            for key in ('captures', 'blurs'):
                assert warm_after[key] == warm_before[key], (key, warm_before, warm_after)
            if preset == 'frosted':
                assert warm_after['material_cache_reuses'] > warm_before['material_cache_reuses'], (warm_before, warm_after)
            else:
                # Liquid redraws its antialiased boundary on full monitor damage;
                # input reuse is the privacy prerequisite, while the ordinary
                # regional suite separately verifies finished-output reuse.
                assert warm_after['partial_materials'] > warm_before['partial_materials'], (warm_before, warm_after)
            records[preset + '-warm-counters'] = {'before': warm_before, 'after': warm_after,
                                                'layers': control('buffers')['buffers']['layers']}
            desktops, captures = [], []
            for name, color in (('red', '#d02020'), ('blue', '#2030d0')):
                color_path.write_text(color)
                ack = Path(str(color_path) + '.ack')
                wait(lambda: ack.exists() and ack.read_text() == color)
                desktops.append(desktop(preset + '-' + name + '-desktop'))
                captures.append(workspace(preset + '-' + name + '-workspace'))
            layer_roi = (100, 94, 660, 188)
            assert ImageChops.difference(desktops[0].crop(layer_roi), desktops[1].crop(layer_roi)).getbbox(), \
                preset + ': secret did not affect the desktop layer backdrop'
            receiver_roi = (270, 270, 620, 410)
            assert ImageChops.difference(desktops[0].crop(receiver_roi), desktops[1].crop(receiver_roi)).getbbox(), \
                preset + ': secret did not affect the desktop window backdrop'
            assert captures[0].tobytes() == captures[1].tobytes(), preset + ': protected window leaked into workspace capture'
            control('apply', patch={})
            rebuilt = workspace(preset + '-rebuilt-workspace')
            assert rebuilt.tobytes() == captures[1].tobytes(), preset + ': cached workspace differs from rebuilt capture'
            layer_config(preset, enabled=False)
            time.sleep(.3)
            native = workspace(preset + '-native-workspace')
            assert native.tobytes() == captures[1].tobytes(), preset + ': isolated capture reused desktop layer glass'
            control('apply', patch={'enabled': True})
            layer_config(preset)
            wait(lambda: any(item['cached'] for item in control('buffers')['buffers']['layers']))
            restored = desktop(preset + '-restored-desktop')
            assert restored.tobytes() == desktops[1].tobytes(), preset + ': workspace capture altered desktop rendering'
            print('PASS: ' + preset + ' warmed window/layer workspace exclusions, native pixels and desktop restoration', flush=True)
        (output / 'workspace-capture.json').write_text(json.dumps(records, indent=2) + '\n')
    finally:
        (output / 'workspace-capture.json').write_text(json.dumps(records, indent=2) + '\n')
        config_path.write_text(saved_config)
        ctl('reload')
        ctl('plugin', 'unload', str(capture_plugin))
