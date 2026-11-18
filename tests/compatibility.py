"""Exercise real rendering, layer hooks, IPC and reloads on each supported host."""
import json
import subprocess
import time

from PIL import Image, ImageChops


def verify_compatibility(ctl, env, log, clients, output, wait, fixtures, plugin, config_path, control):
    def launch(name, opacity, script):
        client = subprocess.Popen([
            'kitty', '--class', name, '-o', 'remember_window_size=no',
            '-o', 'cursor_blink_interval=0', '-o', f'background_opacity={opacity}',
            'python3', '-c', script,
        ], env=env, stdout=log, stderr=log)
        clients.append(client)
        return wait(lambda: next((w for w in json.loads(ctl('-j', 'clients')) if w['class'] == name), None))

    def action(window, operation, arguments):
        ctl('dispatch', 'hl.dsp.window.' + operation + '({ window = ' +
            json.dumps('address:' + window['address']) + ', ' + arguments + ' })')

    backdrop = launch('hyprliquid-background', 1, r'''
import time
print('\x1b[?25l\x1b[2J\x1b[H' + '\n'.join(''.join(
    '\x1b[48;2;%d;%d;%dm  ' % ((180, 35, 25) if (x//4+y//3)%2 else (20, 70, 200))
    for x in range(100)) for y in range(70)), end='', flush=True)
time.sleep(120)
''')
    action(backdrop, 'float', 'action = "enable"')
    action(backdrop, 'resize', 'x = 1100, y = 720, relative = false')
    action(backdrop, 'move', 'x = 30, y = 30, relative = false')
    window = launch('hyprliquid-compatibility', 0.1,
                    "import time; print('\\x1b[?25l\\x1b[2J', end='', flush=True); time.sleep(120)")
    action(window, 'float', 'action = "enable"')
    action(window, 'resize', 'x = 480, y = 340, relative = false')
    action(window, 'move', 'x = 220, y = 180, relative = false')
    action(window, 'alter_zorder', 'mode = "top"')
    owner = 'window:0x' + window['address'].removeprefix('0x')

    def capture(name, crop=True):
        time.sleep(0.35)
        path = output / (name + '.png')
        subprocess.run(['grim', str(path)], env=env, check=True, capture_output=True, timeout=10)
        with Image.open(path) as image:
            pixels = image.convert('RGB')
            assert pixels.getbbox(), name + ': empty framebuffer'
            return pixels.crop((240, 210, 680, 500)) if crop else pixels.copy()

    def apply(**profile):
        result = control('apply', profile=profile)
        assert result['ok'], result
        return result

    def drawn():
        data = control('buffers')['buffers']
        return next((item for item in data['windows'] if item['owner'] == owner and item['material_draws'] > 0), None)

    apply(enabled=False)
    disabled = capture('compat-disabled')
    snapshots = {}
    for preset, model in [('frosted-glass', 'legacy'), ('frosted-liquid-medium', 'liquid')]:
        result = apply(enabled=True, base_preset=preset, theme='dark')
        assert result['state']['model'] == model, result
        wait(drawn)
        dark = capture('compat-' + model + '-dark')
        assert ImageChops.difference(disabled, dark).getbbox(), model + ': glass did not render'
        apply(enabled=True, base_preset=preset, theme='light')
        light = capture('compat-' + model + '-light')
        assert ImageChops.difference(dark, light).getbbox(), model + ': theme did not repaint'
        apply(enabled=True, base_preset=preset, theme='dark')
        restored = capture('compat-' + model + '-restored')
        assert restored.tobytes() == dark.tobytes(), model + ': idle theme round trip changed pixels'
        snapshots[model] = dark
    assert ImageChops.difference(snapshots['legacy'], snapshots['liquid']).getbbox(), 'Material switch did not render'

    action(window, 'fullscreen', 'mode = "fullscreen", action = "set"')
    capture('compat-fullscreen', crop=False)
    action(window, 'fullscreen', 'mode = "fullscreen", action = "unset"')
    action(window, 'resize', 'x = 540, y = 380, relative = false')
    capture('compat-resized', crop=False)
    action(window, 'resize', 'x = 480, y = 340, relative = false')
    action(window, 'move', 'x = 220, y = 180, relative = false')
    print('PASS: both material families, theme round trips, fullscreen and resize', flush=True)

    layer_binary = fixtures / 'layer-test'
    assert layer_binary.exists(), 'Build build/layer-test for the compatibility check'
    original_config = config_path.read_text()
    layer = None
    try:
        for preset, model in [('frosted', 'legacy'), ('frosted-liquid-medium', 'liquid')]:
            config_path.write_text(original_config + '\nhl.plugin.hyprliquid.config({ layers = { enabled = true, mask_mode = "alpha" } })\n'
                'hl.plugin.hyprliquid.layer("hyprliquid-vram-test", { preset = ' + json.dumps(preset) + ' })\n')
            ctl('reload')
            if layer is None:
                layer = subprocess.Popen([str(layer_binary)], env=env | {'GSK_RENDERER': 'cairo', 'GTK_A11Y': 'none'}, stdout=log, stderr=log)
                clients.append(layer)
            wait(lambda: any(item['cached'] and item['model'] == model for item in control('buffers')['buffers']['layers']))
            capture('compat-layer-' + model, crop=False)
        layer.terminate()
        layer.wait(timeout=5)
        wait(lambda: not control('buffers')['buffers']['layers'])
    finally:
        if layer is not None and layer.poll() is None:
            layer.terminate()
            layer.wait(timeout=5)
        config_path.write_text(original_config)
        ctl('reload')
    print('PASS: native layer hooks, both layer materials and close cleanup', flush=True)

    epochs = []
    for cycle in range(2):
        epochs.append(control('state')['state']['epoch'])
        ctl('plugin', 'unload', str(plugin))
        assert not any(p['name'] == 'hyprliquid' for p in json.loads(ctl('-j', 'plugin', 'list')))
        ctl('plugin', 'load', str(plugin))
        state = control('state')['state']
        assert state['epoch'] not in epochs and not state['active'], state
        apply(enabled=True, base_preset='frosted-liquid-medium', theme='dark')
        wait(drawn)
        capture(f'compat-reload-{cycle}')
        assert not ctl('configerrors')
    diagnostic = control('buffers')['buffers']
    assert diagnostic['allocation_failures'] == 0, diagnostic
    (output / 'compatibility.json').write_text(json.dumps({
        'hyprland': json.loads(ctl('-j', 'version')), 'epochs': epochs, 'buffers': diagnostic,
    }, indent=2) + '\n')
    print('PASS: unload/reload with live windows, new IPC epochs and successful rendering', flush=True)
