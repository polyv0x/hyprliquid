"""Real client damage, native raw pixels and dependency checks in nested Hyprland."""
import hashlib
import json
import subprocess
import time


def verify_gpu_cache(ctl, env, log, clients, build, wait, fixtures, isolation_only=False, legacy=False):
    from PIL import Image, ImageChops

    def control(op, **values):
        reply = json.loads(ctl('hyprliquid:control', json.dumps({'op': op, **values})))
        assert reply['ok'], reply
        if op == 'buffers' and 'backdrop_cache' in reply['buffers']:
            cache = reply['buffers']['backdrop_cache']
            assert cache['pairs'] <= cache['retained_limit_pairs'] and cache['bytes'] <= cache['retained_limit_bytes'], cache
            assert cache['leased_bytes'] == 0, cache
        return reply

    def counters():
        fields = ('captures', 'blurs', 'material_draws', 'local_cache_reuses')
        if legacy: fields += ('backdrop_cache_reuses', 'scene_base_cache_reuses', 'window_backdrop_reuses', 'partial_captures', 'partial_blurs', 'material_cache_reuses')
        return {w['owner']: {k: w.get(k, 0) for k in fields}
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
    control('apply', profile={'base_preset': 'frosted-glass' if legacy else 'frosted-liquid', 'theme': 'dark',
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
    neighbor_changed = ImageChops.difference(before_image.crop((30, 40, 510, 340)),
                                           after_image.crop((30, 40, 510, 340))).getbbox()
    if legacy:
        assert neighbor_changed, 'Frosted glass lost its live neighbor refraction'
    else:
        assert not neighbor_changed, 'Neighbor color leaked into local Liquid'
    if isolation_only:
        print('PASS: default Liquid body is isolated from an adjacent client repaint', flush=True)
        return
    for owner in before:
        if legacy:
            delta = {k: after[owner][k] - before[owner][k] for k in before[owner]}
            assert delta['scene_base_cache_reuses'] > 0, delta
            assert delta['material_draws'] + delta['material_cache_reuses'] == delta['captures'] + delta['backdrop_cache_reuses'] + delta['window_backdrop_reuses'], delta
            assert delta['captures'] == delta['blurs'], delta
            if owner == 'window:' + windows[1]['address']:
                assert delta['captures'] == 0 and delta['window_backdrop_reuses'] > 0, delta
        else:
            assert after[owner]['captures'] == before[owner]['captures'], (owner, before, after)
            assert after[owner]['blurs'] == before[owner]['blurs'], (owner, before, after)
            assert after[owner]['material_draws'] == before[owner]['material_draws'], (owner, before, after)
            assert after[owner]['local_cache_reuses'] > before[owner]['local_cache_reuses'], (owner, before, after)
    evidence['client_repaints'] = {'before': before, 'after': after}
    parity('local-dark')
    print('PASS: unequal panes reuse rendered base glass and retain live frosted-glass refraction' if legacy else
          'PASS: unequal local panes ignore foreground damage, isolate neighbors and reuse exact raw material output', flush=True)

    if legacy:
        action(windows[2], 'resize', 'x = 430, y = 150, relative = false')
        action(windows[2], 'move', 'x = 40, y = 580, relative = false')
        time.sleep(.3)
        before = counters()
        write(paths[1], [90, 210, 130]); time.sleep(.2)
        after = counters()
        distant = 'window:' + windows[2]['address']
        assert after[distant]['captures'] == before[distant]['captures'], (before, after)
        assert after[distant]['window_backdrop_reuses'] > before[distant]['window_backdrop_reuses'], (before, after)
        adjacent = 'window:' + windows[0]['address']
        assert after[adjacent]['partial_captures'] > before[adjacent]['partial_captures'], (before, after)
        assert after[adjacent]['partial_blurs'] > before[adjacent]['partial_blurs'], (before, after)
        evidence['spatial_dependencies'] = {'before': before, 'after': after}
        parity('outside-sampling-region')
        # Changes made while the overlap fallback is active must not resurrect
        # the old retained input when the original layout returns.
        action(windows[0], 'move', 'x = 650, y = 100, relative = false')
        time.sleep(.2)
        write(paths[1], [160, 80, 230]); time.sleep(.2)
        action(windows[0], 'move', 'x = 30, y = 40, relative = false')
        time.sleep(.3)
        parity('resume-after-overlap')
        workspace = str(windows[0]['workspace']['id'])
        ctl('dispatch', 'hl.dsp.focus({ workspace = "99" })')
        time.sleep(.2)
        write(paths[1], [210, 170, 60]); time.sleep(.2)
        ctl('dispatch', 'hl.dsp.focus({ workspace = ' + json.dumps(workspace) + ' })')
        time.sleep(.3)
        parity('resume-after-hidden-damage')
        action(windows[2], 'resize', 'x = 430, y = 310, relative = false')
        action(windows[2], 'move', 'x = 40, y = 420, relative = false')
        time.sleep(.3)
        print('PASS: distant changes skip blur; adjacent damage updates regions; fallback transitions rebuild stale inputs', flush=True)

    def layer(bottom=False):
        proc = subprocess.Popen([str(fixtures / 'layer-test'), '--animate', *(['--bottom'] if bottom else [])],
                                env=env | {'GSK_RENDERER': 'cairo', 'GTK_A11Y': 'none'}, stdout=log, stderr=log)
        clients.append(proc)
        wait(lambda: 'hyprliquid-vram-test' in ctl('-j', 'layers'))
        return proc

    top = layer()
    time.sleep(.5)
    before = counters()
    before_cache = control('buffers')['buffers'].get('backdrop_cache')
    time.sleep(.5)
    after = counters()
    if legacy:
        after_cache = control('buffers')['buffers']['backdrop_cache']
        assert after_cache['misses'] == before_cache['misses'], (before_cache, after_cache)
        assert all(after[k]['captures'] == before[k]['captures'] for k in before), (before, after)
        assert any(after[k]['window_backdrop_reuses'] > before[k]['window_backdrop_reuses'] for k in before)
    else:
        assert all(after[k]['captures'] == before[k]['captures'] for k in before), (before, after)
        assert any(after[k]['local_cache_reuses'] > before[k]['local_cache_reuses'] for k in before)
    top.terminate(); top.wait(timeout=5)
    wait(lambda: 'hyprliquid-vram-test' not in ctl('-j', 'layers'))
    bottom = layer(True)
    time.sleep(.5)
    before = counters()
    before_cache = control('buffers')['buffers'].get('backdrop_cache')
    time.sleep(.5)
    after = counters()
    assert all(after[k]['captures'] > before[k]['captures'] for k in before), (before, after)
    if legacy:
        assert control('buffers')['buffers']['backdrop_cache']['misses'] > before_cache['misses']
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
    if legacy:
        for sigma in (0, 19.8):
            for strength in (0, .5, 1):
                control('apply', patch={'overrides': {'blur_sigma': sigma, 'neighbor_contribution': strength}})
                time.sleep(.25)
                parity(f'legacy-sigma-{sigma}-contribution-{strength}')
        control('apply', patch={'overrides': {'blur_sigma': 0}})
        action(windows[0], 'resize', 'x = 460, y = 300, relative = false')
        time.sleep(.25)
        parity('legacy-zero-blur-resize')
        action(windows[0], 'resize', 'x = 480, y = 300, relative = false')
        control('apply', patch={'reset_fields': ['blur_sigma', 'neighbor_contribution']})
    monitor = json.loads(ctl('-j', 'monitors'))[0]['name']
    for index, (scale, transform) in enumerate(((1.25, 0), (1.6, 0), (1, 1), (1.25, 3))):
        ctl('eval', 'hl.monitor({ output = ' + json.dumps(monitor) +
            f', mode = "1280x800@60", scale = {scale}, transform = {transform}' + ' })')
        time.sleep(.5)
        if legacy:
            write(paths[1], [140 + index * 10, 40 + index * 30, 220 - index * 20]); time.sleep(.2)
        parity(f'scale-{scale}-transform-{transform}')
    ctl('eval', 'hl.monitor({ output = ' + json.dumps(monitor) +
        ', mode = "1280x800@60", scale = 1, transform = 0, bitdepth = 10, cm = "srgb" })')
    time.sleep(.4)
    if legacy:
        write(paths[1], [10, 200, 210]); time.sleep(.2)
    parity('sdr-10bit')
    ctl('eval', 'hl.config({ render = { use_fp16 = 1 } })')
    time.sleep(.4)
    if legacy:
        write(paths[1], [160, 20, 100]); time.sleep(.2)
    parity('sdr-fp16')
    ctl('eval', 'hl.monitor({ output = ' + json.dumps(monitor) +
        ', mode = "1280x800@60", scale = 1, transform = 0, bitdepth = 10, cm = "hdr", supports_hdr = 1, supports_wide_color = 1 })')
    time.sleep(.4)
    if legacy:
        write(paths[1], [190, 180, 50]); time.sleep(.2)
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
    assert all(after[k]['material_draws'] + after[k].get('material_cache_reuses', 0) >
               before[k]['material_draws'] + before[k].get('material_cache_reuses', 0) for k in before)
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
    retirement_start = control('buffers')['buffers']['frame']
    started = last_draw = time.monotonic()
    frame = retirement_start
    timeline = []
    while True:
        retired = control('buffers')['buffers']
        now = time.monotonic()
        if retired['frame'] != frame:
            frame, last_draw = retired['frame'], now
        timeline.append({'seconds': now - started, 'frame': frame,
                         'scratch_bytes': retired['scratch_pool']['bytes'],
                         'backdrop_bytes': retired.get('backdrop_cache', {}).get('bytes', 0)})
        if not timeline[-1]['scratch_bytes'] and not timeline[-1]['backdrop_bytes']:
            break
        # Output removal can deliver delayed configure/commit frames. Measure
        # the five-second idle lifetime from the last draw, with timer slack.
        if now - last_draw > 7 or now - started > 15:
            break
        time.sleep(.2)
    evidence['retirement'] = {'start_frame': retirement_start, 'timeline': timeline, 'after': retired}
    (build / 'gpu-cache.json').write_text(json.dumps(evidence, indent=2))
    assert retired['scratch_pool']['bytes'] == 0
    if 'material_cache' in retired:
        assert retired['material_cache']['bytes'] == 0
    if 'backdrop_cache' in retired:
        assert retired['backdrop_cache']['bytes'] == 0
    assert not any(b['owner'] == f'monitor:{monitor_id}' for b in retired['buffers'])
    time.sleep(.5)
    assert control('buffers')['buffers']['frame'] == retired['frame'], 'Cleanup scheduled another frame'
    print('PASS: independent mixed-scale SDR/HDR outputs reuse exact scratch sizes and retire idle/removed storage', flush=True)
    (build / 'gpu-cache.json').write_text(json.dumps(evidence, indent=2))
    print('PASS: settled glass schedules no continuing frames', flush=True)
