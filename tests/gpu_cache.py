"""Real client damage, native raw pixels and dependency checks in nested Hyprland."""
import hashlib
import json
import subprocess
import time


def verify_gpu_cache(ctl, env, log, clients, build, wait, fixtures, isolation_only=False):
    from PIL import Image, ImageChops

    def control(op, **values):
        reply = json.loads(ctl('hyprliquid:control', json.dumps({'op': op, **values})))
        assert reply['ok'], reply
        return reply

    def counters():
        return {w['owner']: {k: w[k] for k in ('captures', 'blurs', 'material_draws', 'local_cache_reuses')}
                for w in control('buffers')['buffers']['windows'] if w['enabled']}

    def action(window, name, fields):
        ctl('dispatch', 'hl.dsp.window.' + name + '({ window = ' + json.dumps('address:' + window['address']) + ', ' + fields + ' })')

    def write(path, color):
        temporary = path.with_suffix('.tmp')
        temporary.write_text(json.dumps(color))
        temporary.replace(path)

    def png(name):
        path = build / (name + '.png')
        subprocess.run(['grim', str(path)], env=env, check=True, capture_output=True, timeout=10)
        return Image.open(path).convert('RGB')

    def raw(name):
        path = build / name
        ctl('hyprliquid-test:capture', str(path))
        metadata_path = path.parent / (path.name + '.json')
        wait(lambda: metadata_path.exists())
        metadata = json.loads(metadata_path.read_text())
        assert metadata['gl_error'] == 0, metadata
        return (path.parent / (path.name + '.raw')).read_bytes()

    def parity(name):
        cached = raw(name + '-cached')
        control('apply', patch={})  # Invalidate through the public runtime path.
        time.sleep(.25)
        rebuilt = raw(name + '-rebuilt')
        assert cached == rebuilt, f'Cached output differs from recomputation: {name}'
        evidence['raw_parity'][name] = hashlib.sha256(cached).hexdigest()

    ctl('reload')
    ctl('dismissnotify')
    ctl('eval', 'hl.config({ general = { border_size = 0, gaps_in = 8, gaps_out = 20 }, '
        'animations = { enabled = false }, decoration = { active_opacity = 1, inactive_opacity = 1, '
        'dim_inactive = false }, cursor = { no_warps = true, inactive_timeout = 0.1 } })')
    ctl('plugin', 'load', str(fixtures / 'capture-test.so'))
    control('apply', profile={'base_preset': 'frosted-liquid', 'theme': 'dark',
                             'animate_display_angle': False, 'animate_hover_angle': False})
    evidence = {'raw_parity': {}}
    code = r'''
import json, shutil, sys, time
from pathlib import Path
path = Path(sys.argv[1]); previous = None
while True:
    color = json.loads(path.read_text()); size = shutil.get_terminal_size()
    if (color, size) != previous:
        previous = (color, size)
        out = '\x1b[?25l\x1b[0m\x1b[2J'
        for y in range(size.lines):
            if y % 6 < 3:
                out += '\x1b[%d;1H\x1b[48;2;%d;%d;%dm' % (y+1, *color) + ' ' * (size.columns-1) + '\x1b[0m'
        print(out, end='', flush=True)
    time.sleep(.01)
'''
    paths = []
    windows = []
    for i, (x, y, w, h) in enumerate(((30, 40, 480, 300), (526, 50, 700, 270),
                                     (40, 420, 430, 310), (550, 400, 620, 290))):
        path = build / f'gpu-content-{i}.json'
        write(path, [20, 50, 180])
        paths.append(path)
        client = subprocess.Popen(['kitty', '--start-as=normal', '--class', f'gpu-pane-{i}',
            '-o', 'background_opacity=0', '-o', 'remember_window_size=no', '-o', 'cursor_blink_interval=0',
            'python3', '-c', code, str(path)], env=env, stdout=log, stderr=log)
        clients.append(client)
        window = wait(lambda: next((c for c in json.loads(ctl('-j', 'clients')) if c['class'] == f'gpu-pane-{i}'), None))
        windows.append(window)
        action(window, 'float', 'action = "enable"')
        action(window, 'resize', f'x = {w}, y = {h}, relative = false')
        action(window, 'move', f'x = {x}, y = {y}, relative = false')
    time.sleep(1)
    before_image = png('local-before')
    before = None if isolation_only else counters()
    for i in range(10):
        write(paths[1], [220 if i % 2 else 30, 160, 20])
        time.sleep(.04)
    time.sleep(.2)
    after = None if isolation_only else counters()
    after_image = png('local-after')
    assert ImageChops.difference(before_image, after_image).getbbox(), 'Animated client did not repaint'
    # The neighboring pane's complete body, including its filter-sensitive edge.
    assert not ImageChops.difference(before_image.crop((30, 40, 510, 340)),
                                    after_image.crop((30, 40, 510, 340))).getbbox(), 'Neighbor color leaked into local Liquid'
    if isolation_only:
        print('PASS: default Liquid body is isolated from an adjacent client repaint', flush=True)
        return
    for owner in before:
        assert after[owner]['captures'] == before[owner]['captures'], (owner, before, after)
        assert after[owner]['blurs'] == before[owner]['blurs'], (owner, before, after)
        assert after[owner]['material_draws'] == before[owner]['material_draws'], (owner, before, after)
        assert after[owner]['local_cache_reuses'] > before[owner]['local_cache_reuses'], (owner, before, after)
    evidence['client_repaints'] = {'before': before, 'after': after}
    parity('local-dark')
    print('PASS: unequal local panes ignore foreground damage, isolate neighbors and reuse exact raw material output', flush=True)

    def layer(bottom=False):
        proc = subprocess.Popen([str(fixtures / 'layer-test'), '--animate', *(['--bottom'] if bottom else [])],
                                env=env | {'GSK_RENDERER': 'cairo', 'GTK_A11Y': 'none'}, stdout=log, stderr=log)
        clients.append(proc)
        wait(lambda: 'hyprliquid-vram-test' in ctl('-j', 'layers'))
        return proc

    top = layer()
    time.sleep(.5)
    before = counters()
    time.sleep(.5)
    after = counters()
    assert all(after[k]['captures'] == before[k]['captures'] for k in before), (before, after)
    assert any(after[k]['local_cache_reuses'] > before[k]['local_cache_reuses'] for k in before)
    top.terminate(); top.wait(timeout=5)
    wait(lambda: 'hyprliquid-vram-test' not in ctl('-j', 'layers'))
    bottom = layer(True)
    time.sleep(.5)
    before = counters()
    time.sleep(.5)
    after = counters()
    assert all(after[k]['captures'] > before[k]['captures'] for k in before), (before, after)
    evidence['bottom_repaints'] = {'before': before, 'after': after}
    bottom.terminate(); bottom.wait(timeout=5)
    wait(lambda: 'hyprliquid-vram-test' not in ctl('-j', 'layers'))
    time.sleep(.4)
    parity('bottom-closed')
    print('PASS: animated top layers reuse glass; bottom layers invalidate it and leave no stale backdrop', flush=True)

    for theme in ('light', 'dark'):
        for opacity in (1, .73):
            control('apply', patch={'theme': theme, 'overrides': {'glass_opacity': opacity}})
            time.sleep(.25)
            parity(f'{theme}-opacity-{opacity}')
    monitor = json.loads(ctl('-j', 'monitors'))[0]['name']
    for scale, transform in ((1.25, 0), (1.6, 0), (1, 1), (1.25, 3)):
        ctl('eval', 'hl.monitor({ output = ' + json.dumps(monitor) +
            f', mode = "1280x800@60", scale = {scale}, transform = {transform}' + ' })')
        time.sleep(.5)
        parity(f'scale-{scale}-transform-{transform}')
    ctl('eval', 'hl.monitor({ output = ' + json.dumps(monitor) +
        ', mode = "1280x800@60", scale = 1, transform = 0, bitdepth = 10, cm = "srgb" })')
    time.sleep(.4)
    parity('sdr-10bit')
    ctl('eval', 'hl.config({ render = { use_fp16 = 1 } })')
    time.sleep(.4)
    parity('sdr-fp16')
    ctl('eval', 'hl.monitor({ output = ' + json.dumps(monitor) +
        ', mode = "1280x800@60", scale = 1, transform = 0, bitdepth = 10, cm = "hdr", supports_hdr = 1, supports_wide_color = 1 })')
    time.sleep(.4)
    parity('hdr-fp16')
    ctl('eval', 'hl.config({ render = { use_fp16 = 2 } }); hl.monitor({ output = ' + json.dumps(monitor) +
        ', mode = "1280x800@60", scale = 1, transform = 0, bitdepth = 8, cm = "srgb", supports_hdr = 0, supports_wide_color = 0 })')
    time.sleep(.4)
    print('PASS: exact cached/rebuilt raw pixels with fractional scale, transforms, clipped panes, SDR 10-bit and FP16/HDR', flush=True)
    control('apply', patch={'overrides': {'glass_opacity': 1, 'outer_refraction_amount': 45}})
    time.sleep(.3)
    before = counters()
    warm_buffers = control('buffers')['buffers']
    for i in range(10):
        write(paths[1], [80, 20 if i % 2 else 100, 230]); time.sleep(.04)
    time.sleep(.2)
    after = counters()
    steady_buffers = control('buffers')['buffers']
    assert steady_buffers['allocations'] == warm_buffers['allocations'], (warm_buffers, steady_buffers)
    assert steady_buffers['scratch_pool']['misses'] == warm_buffers['scratch_pool']['misses']
    assert steady_buffers['scratch_pool']['hits'] > warm_buffers['scratch_pool']['hits']
    evidence['scratch_warm'] = warm_buffers
    evidence['scratch_steady'] = steady_buffers
    assert all(after[k]['material_draws'] > before[k]['material_draws'] for k in before)
    control('apply', patch={'reset_fields': ['outer_refraction_amount']})
    action(windows[0], 'move', 'x = 650, y = 100, relative = false')
    action(windows[0], 'alter_zorder', 'mode = "top"')
    time.sleep(.4)
    before = counters()
    write(paths[1], [20, 230, 60]); time.sleep(.3)
    after = counters()
    assert all(after[k]['material_draws'] > before[k]['material_draws'] for k in before)
    parity('overlap')
    print('PASS: outward rays and real underlays retain live scene dependencies', flush=True)
    action(windows[0], 'move', 'x = 30, y = 40, relative = false')
    time.sleep(1)
    before_frame = control('buffers')['buffers']['frame']
    time.sleep(.5)
    after_frame = control('buffers')['buffers']['frame']
    assert after_frame == before_frame, (before_frame, after_frame)
    evidence['idle_frames'] = after_frame - before_frame
    secondary = 'HYPRLIQUID-MIXED'
    ctl('output', 'create', 'headless', secondary)
    ctl('eval', 'hl.monitor({ output = "' + secondary + '", mode = "800x600@60", position = "-640x0", '
        'scale = 1.25, transform = 0, bitdepth = 10, cm = "hdr", supports_hdr = 1, supports_wide_color = 1 }); '
        'hl.window_rule({ name = "gpu-second-output", match = { class = "^gpu-second$" }, workspace = "22" })')
    assert not ctl('configerrors')
    secondary_path = build / 'gpu-content-secondary.json'
    write(secondary_path, [100, 60, 20])
    second_client = subprocess.Popen(['kitty', '--start-as=normal', '--class', 'gpu-second',
        '-o', 'background_opacity=0', '-o', 'remember_window_size=no', '-o', 'cursor_blink_interval=0',
        'python3', '-c', code, str(secondary_path)], env=env, stdout=log, stderr=log)
    clients.append(second_client)
    second = wait(lambda: next((c for c in json.loads(ctl('-j', 'clients')) if c['class'] == 'gpu-second'), None))
    ctl('dispatch', 'hl.dsp.workspace.move({ workspace = "22", monitor = "' + secondary + '" })')
    ctl('dispatch', 'hl.dsp.focus({ window = ' + json.dumps('address:' + windows[0]['address']) + ' })')
    time.sleep(.6)
    before = counters()
    for i in range(10):
        write(paths[1], [220 if i % 2 else 30, 160, 20]); time.sleep(.04)
    time.sleep(.2)
    after = counters()
    second_owner = 'window:0x' + second['address'].removeprefix('0x')
    assert after[second_owner]['captures'] == before[second_owner]['captures'], (before, after)
    control('apply', patch={'overrides': {'outer_refraction_amount': 45}})
    for i in range(12):
        write(paths[1], [80, 20 if i % 2 else 100, 230])
        write(secondary_path, [200, 30 if i % 2 else 180, 40]); time.sleep(.04)
    warm = control('buffers')['buffers']
    for i in range(12):
        write(paths[1], [80, 20 if i % 2 else 100, 230])
        write(secondary_path, [200, 30 if i % 2 else 180, 40]); time.sleep(.04)
    steady = control('buffers')['buffers']
    assert steady['allocations'] == warm['allocations'], (warm, steady)
    assert len({b['drm_format'] for b in steady['buffers'] if b['role'] == 'window-sharp'}) >= 2
    evidence['mixed_outputs'] = {'warm': warm, 'steady': steady}
    second_client.terminate(); second_client.wait(timeout=5)
    monitor_id = next(m['id'] for m in json.loads(ctl('-j', 'monitors')) if m['name'] == secondary)
    ctl('output', 'remove', secondary)
    control('apply', patch={'reset_fields': ['outer_refraction_amount']})
    time.sleep(6.2)
    retired = control('buffers')['buffers']
    assert retired['scratch_pool']['bytes'] == 0
    assert not any(b['owner'] == f'monitor:{monitor_id}' for b in retired['buffers'])
    print('PASS: independent mixed-scale SDR/HDR outputs reuse exact scratch sizes and retire idle/removed storage', flush=True)
    (build / 'gpu-cache.json').write_text(json.dumps(evidence, indent=2))
    print('PASS: settled local glass schedules no continuing frames', flush=True)
