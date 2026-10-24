"""Liquid material integration in the disposable compositor only."""
import json
import math
import statistics
import subprocess
import time


def verify_liquid(ctl, env, log, clients, build, wait):
    from PIL import Image, ImageChops
    for client in clients:
        if client.poll() is None:
            client.terminate()
            client.wait(timeout=5)
    wait(lambda: not json.loads(ctl('-j', 'clients')))
    ctl('reload')
    ctl('dismissnotify')
    monitor = json.loads(ctl('-j', 'monitors'))[0]['name']
    ctl('eval', 'hl.config({ general = { border_size = 0, gaps_in = 8, gaps_out = 20 }, '
        'decoration = { active_opacity = 1, inactive_opacity = 1, dim_inactive = false, shadow = { enabled = true } }, '
        'cursor = { no_warps = true, inactive_timeout = 0.1 } })')

    def control(op, **fields):
        reply = json.loads(ctl('hyprliquid:control', json.dumps({'op': op, **fields})))
        assert reply['ok'], reply
        return reply

    def action(window, name, fields):
        ctl('dispatch', 'hl.dsp.window.' + name + '({ window = ' + json.dumps('address:' + window['address']) + ', ' + fields + ' })')

    def capture(name):
        time.sleep(0.3)
        path = build / (name + '.png')
        subprocess.run(['grim', str(path)], env=env, check=True, capture_output=True, timeout=10)
        return Image.open(path).convert('RGB')

    def focus(window):
        ctl('dispatch', 'hl.dsp.focus({ window = ' + json.dumps('address:' + window['address']) + ' })')

    def crop(image, box, scale):
        return image.crop(tuple(round(x * scale) for x in box))

    def max_delta(a, b):
        return max(max(pixel) for pixel in ImageChops.difference(a, b).getdata())

    code = r'''
import json, shutil, sys, time
from pathlib import Path
path = Path(sys.argv[1]); last = None
while True:
    data = json.loads(path.read_text()); size = shutil.get_terminal_size()
    if (data, size) != last:
        last = (data, size)
        out = '\x1b[?25l\x1b[0m\x1b[2J'
        for y in range(size.lines):
            out += '\x1b[%d;1H' % (y + 1)
            if not data.get('empty') and (data.get('solid') or y // 3 % 2 == 0):
                out += '\x1b[48;2;%d;%d;%dm' % tuple(data['color']) + ' ' * (size.columns - 1) + '\x1b[0m'
        print(out, end='', flush=True)
    time.sleep(0.05)
'''
    paths = []
    control('apply', profile={'base_preset': 'frosted-liquid-medium', 'theme': 'dark'})
    for i in range(2):
        path = build / f'liquid-content-{i}.json'
        path.write_text(json.dumps({'empty': i == 0, 'color': [20, 60, 230]}))
        paths.append(path)
        clients.append(subprocess.Popen(['kitty', '--start-as=normal', '--class', f'liquid-{i}',
            '-o', 'remember_window_size=no', '-o', 'background_opacity=0', '-o', 'font_size=16',
            'python3', '-c', code, str(path)], env=env, stdout=log, stderr=log))
        wait(lambda: len(json.loads(ctl('-j', 'clients'))) == i + 1)

    # Negative inner refraction stays inside the pane. Include its outer-ray
    # edge pixels, and ensure exterior shadows never copy neighboring content.
    for scale, transform in ((1, 0), (1.5, 0), (2, 0), (1.25, 0), (1, 1)):
        mode = '1440x900@60' if scale == 1.5 else '1280x800@60'
        ctl('eval', 'hl.monitor({ output = ' + json.dumps(monitor) +
            f', mode = "{mode}", position = "auto", scale = {scale}, transform = {transform}' + ' })')
        time.sleep(0.5)
        assert json.loads(ctl('-j', 'monitors'))[0]['scale'] == scale, 'Compositor substituted a different scale'
        assert control('state')['state']['model'] == 'liquid'
        windows = sorted(json.loads(ctl('-j', 'clients')), key=lambda w: tuple(w['at']))
        (build / f'liquid-scene-geometry-{scale}-{transform}.json').write_text(json.dumps(windows, indent=2))
        receiver, neighbor = windows
        index = int(neighbor['class'].rsplit('-', 1)[1])
        other = 1 - index
        paths[other].write_text(json.dumps({'empty': True, 'color': [0, 0, 0]}))
        x, y = receiver['at']; w, h = receiver['size']
        region = (x + 1, y + 40, x + w - 1, y + h - 10)
        for strength, falloff in ((0, 0), (0.5, 0), (1, 0), (0, 1), (0.5, 1), (1, 1)):
            control('apply', patch={'overrides': {'neighbor_contribution': strength, 'neighbor_falloff': falloff}})
            paths[index].write_text(json.dumps({'color': [20, 60, 230]}))
            focus(receiver); before = capture(f'liquid-scene-{scale}-{transform}-{strength}-{falloff}')
            focus(neighbor); after = capture('liquid-focus')
            # Fractional corner coverage can round source-over shadows one
            # code value differently. The fully covered body must be exact.
            assert max_delta(crop(before, region, scale), crop(after, region, scale)) <= 1, 'Liquid changed on focus'
            core = (x + 36, y + 40, x + w - 36, y + h - 36)
            assert max_delta(crop(before, core, scale), crop(after, core, scale)) == 0, 'Liquid body changed on focus'
            paths[index].write_text(json.dumps({'color': [20, 220, 40]}))
            updated = capture('liquid-neighbor-update')
            if strength == 0:
                assert max_delta(crop(after, region, scale), crop(updated, region, scale)) <= 1, 'Liquid leaked disabled neighbor'
            if neighbor['at'][0] >= x + w:
                gap = (x + w + 2, y + 45, min(x + w + 8, neighbor['at'][0] - 1), y + h - 45)
            else:
                gap = (x + 45, y + h + 2, x + w - 45, min(y + h + 8, neighbor['at'][1] - 1))
            assert max_delta(crop(after, gap, scale), crop(updated, gap, scale)) == 0, f'Liquid shadow copied neighbor content: scale {scale}, gap {gap}'
        print(f'PASS: Liquid neighbor/falloff focus, isolation and local shadows, scale {scale}, transform {transform}', flush=True)

    # Exercise actual live controls over a patterned underlay and require an
    # exact return to the preset after every edit.
    ctl('eval', 'hl.monitor({ output = ' + json.dumps(monitor) + ', mode = "1280x800@60", position = "auto", scale = 1, transform = 0 })')
    time.sleep(0.5)
    receiver, underlay = sorted(json.loads(ctl('-j', 'clients')), key=lambda w: w['class'])
    paths[0].write_text(json.dumps({'empty': True, 'color': [0, 0, 0]}))
    paths[1].write_text(json.dumps({'color': [230, 60, 20]}))
    for window, size, position in ((underlay, (700, 540), (50, 60)), (receiver, (320, 280), (260, 190))):
        action(window, 'float', 'action = "enable"')
        action(window, 'resize', f'x = {size[0]}, y = {size[1]}, relative = false')
        action(window, 'move', f'x = {position[0]}, y = {position[1]}, relative = false')
    action(receiver, 'alter_zorder', 'mode = "top"')
    control('apply', profile={'base_preset': 'frosted-liquid-medium', 'theme': 'dark'})
    baseline = capture('liquid-controls-default')
    edits = {'blur_sigma': 0, 'blur_alpha0': 0, 'blur_alpha3': 0, 'blur_dist3': 25,
             'inner_refraction_amount': -20, 'inner_refraction_inv_height': 0.2,
             'outer_refraction_amount': 45, 'refraction_threshold0': 8, 'refraction_threshold1': 40,
             'refraction_opacity': 0, 'face_opacity': 0, 'rim_width': 5, 'rim_opacity': 0,
             'shadow_amount': 0.6, 'shadow_offset': -20, 'display_angle_degrees': 45}
    for name, value in edits.items():
        control('apply', patch={'overrides': {name: value}})
        changed = capture('liquid-control-' + name)
        assert max_delta(baseline.crop((140, 80, 700, 590)), changed.crop((140, 80, 700, 590))) > 0, f'Liquid control did not render: {name}'
        control('apply', patch={'reset_fields': [name]})
        restored = capture('liquid-control-reset')
        assert max_delta(baseline, restored) == 0, f'Liquid reset changed preset pixels: {name}'
    print('PASS: Liquid live blur/refraction/face/highlight/shadow edits and pixel-identical preset resets', flush=True)

    # With the toggle on, a nonzero target leaves idle pixels unchanged. The
    # angle follows native resize easing, then returns exactly to the baseline.
    control('apply', patch={'animate_display_angle': True, 'overrides': {'display_angle_degrees': 45}})
    assert max_delta(baseline, capture('liquid-angle-idle')) == 0
    owner = 'window:0x' + receiver['address'].removeprefix('0x')

    def angle_sample():
        window = next(w for w in control('buffers')['buffers']['windows'] if w['owner'] == owner)
        return {'box': window['pixel_box'], 'angle': window['rendered_display_angle_degrees']}

    recordings = {}
    for curve, specification in (
        ('ease', 'bezier = "angle-ease"'), ('spring', 'spring = "angle-spring"')):
        ctl('eval', 'hl.curve("angle-ease", { type = "bezier", points = { {0.42, 0}, {0.58, 1} } }); '
            'hl.curve("angle-spring", { type = "spring", mass = 1, stiffness = 65, dampening = 13 }); '
            'hl.config({ animations = { enabled = true } }); '
            'hl.animation({ leaf = "windowsMove", enabled = true, speed = 18, ' + specification + ' })')
        assert not ctl('configerrors')
        action(receiver, 'resize', 'x = 520, y = 400, relative = false')
        samples = []
        deadline = time.monotonic() + 4
        while time.monotonic() < deadline:
            sample = angle_sample()
            samples.append(sample)
            if len(samples) > 5 and abs(sample['box'][2] - 520) < 0.01 and sample['angle'] == 0:
                break
            time.sleep(0.015)
        assert max(s['angle'] for s in samples) > 35, (curve, samples)
        assert samples[-1]['angle'] == 0, (curve, samples[-1])
        for sample in samples:
            progress = max(0, min(1, (sample['box'][2] - 320) / 200))
            assert 0 <= sample['angle'] <= 45
            # IPC can run after a size tick but before that frame is drawn.
            assert abs(sample['angle'] - 45 * math.sin(math.pi * progress)) < 6, (curve, sample, progress)
        recordings[curve] = samples
        ctl('eval', 'hl.config({ animations = { enabled = false } })')
        action(receiver, 'resize', 'x = 320, y = 280, relative = false')
        assert max_delta(baseline, capture('liquid-angle-rest-' + curve)) == 0

    # A fast retarget must continue from the last angle instead of resetting.
    ctl('eval', 'hl.config({ animations = { enabled = true } }); '
        'hl.animation({ leaf = "windowsMove", enabled = true, speed = 18, bezier = "angle-ease" })')
    action(receiver, 'resize', 'x = 520, y = 400, relative = false')
    wait(lambda: angle_sample()['angle'] > 20)
    before = angle_sample()
    action(receiver, 'resize', 'x = 360, y = 300, relative = false')
    after = angle_sample()
    assert abs(after['angle'] - before['angle']) < 6, (before, after)
    recordings['retarget'] = [before, after]
    wait(lambda: angle_sample()['angle'] == 0 and abs(angle_sample()['box'][2] - 360) < 0.01)
    # Disabling native animations must warp immediately, without a lingering tilt.
    ctl('eval', 'hl.config({ animations = { enabled = false } })')
    action(receiver, 'resize', 'x = 320, y = 280, relative = false')
    assert max_delta(baseline, capture('liquid-angle-rest-retarget')) == 0
    control('apply', patch={'animate_display_angle': False})
    assert max_delta(baseline, capture('liquid-angle-static')) > 0
    assert angle_sample()['angle'] == 45
    control('apply', patch={'animate_display_angle': None, 'reset_fields': ['display_angle_degrees']})
    assert max_delta(baseline, capture('liquid-angle-reset')) == 0
    (build / 'liquid-angle-animation.json').write_text(json.dumps(recordings, indent=2))
    print('PASS: Liquid static angle, idle identity, native bezier/spring resize sweep, retarget continuity and exact rest/reset pixels', flush=True)

    # Opaque underlays must survive all contributions at both zero and normal blur.
    for scale, transform in ((1.25, 0), (1, 1)):
        ctl('eval', 'hl.monitor({ output = ' + json.dumps(monitor) +
            f', mode = "1280x800@60", position = "auto", scale = {scale}, transform = {transform}' + ' })')
        time.sleep(0.5)
        windows = sorted(json.loads(ctl('-j', 'clients')), key=lambda w: w['class'])
        receiver, underlay = windows
        paths[0].write_text(json.dumps({'empty': True, 'color': [0, 0, 0]}))
        paths[1].write_text(json.dumps({'solid': True, 'color': [20, 60, 230]}))
        for window, size, position in ((underlay, (700, 540), (50, 60)), (receiver, (320, 280), (260, 190))):
            action(window, 'float', 'action = "enable"')
            action(window, 'resize', f'x = {size[0]}, y = {size[1]}, relative = false')
            action(window, 'move', f'x = {position[0]}, y = {position[1]}, relative = false')
        action(receiver, 'alter_zorder', 'mode = "top"')
        for sigma in (6, 0):
            reference = None
            for strength in (1, 0.5, 0):
                control('apply', patch={'overrides': {'blur_sigma': sigma, 'neighbor_contribution': strength, 'neighbor_falloff': 0}})
                current = crop(capture(f'liquid-underlay-{scale}-{transform}-{sigma}-{strength}'), (240, 240, 300, 400), scale)
                if reference is None:
                    reference = current
                    assert all(b > max(r, g) + 50 for r, g, b in current.getdata()), 'Liquid underlay fixture absent'
                else:
                    assert max_delta(reference, current) <= 1, 'Liquid body/shadow exposed wallpaper through underlay'
        diagnostic = control('buffers')['buffers']
        assert any(b['role'] == 'window-clean-material-backdrop' for b in diagnostic['buffers'])
        assert all(w['owns_shadow'] for w in diagnostic['windows'] if w['enabled'])
    print('PASS: Liquid body and local shadows preserve opaque underlays at partial/zero contribution', flush=True)

    ctl('eval', 'hl.monitor({ output = ' + json.dumps(monitor) + ', mode = "1280x800@60", position = "auto", scale = 1, transform = 0 })')
    time.sleep(0.5)
    # Native shadow enablement must have no visible effect on Liquid windows.
    control('apply', profile={'base_preset': 'frosted-liquid-medium', 'theme': 'light'})
    before = capture('liquid-shadow-native-on')
    ctl('eval', 'hl.config({ decoration = { shadow = { enabled = false } } })')
    after = capture('liquid-shadow-native-off')
    assert max_delta(before.crop((0, 80, 1280, 800)), after.crop((0, 80, 1280, 800))) == 0, 'Double native/material shadow'
    ctl('eval', 'hl.config({ decoration = { shadow = { enabled = true } } })')
    # Compare bounds cleanup against a full repaint, before and after close.
    action(receiver, 'move', 'x = 700, y = 400, relative = false')
    moved = capture('liquid-shadow-moved')
    control('apply', patch={})
    repainted = capture('liquid-shadow-moved-repaint')
    assert max_delta(moved.crop((100, 120, 650, 550)), repainted.crop((100, 120, 650, 550))) == 0, 'Shadow movement trails'
    action(receiver, 'resize', 'x = 180, y = 140, relative = false')
    resized = capture('liquid-shadow-resized')
    control('apply', patch={})
    assert max_delta(resized.crop((600, 300, 1200, 780)), capture('liquid-shadow-resize-repaint').crop((600, 300, 1200, 780))) == 0
    action(receiver, 'fullscreen', 'mode = "fullscreen", action = "set"')
    capture('liquid-fullscreen')
    action(receiver, 'fullscreen', 'mode = "fullscreen", action = "unset"')
    action(receiver, 'move', 'x = -40, y = -40, relative = false')
    capture('liquid-offscreen')
    clients[-2].terminate(); clients[-2].wait(timeout=5)
    wait(lambda: len(json.loads(ctl('-j', 'clients'))) == 1)
    closed = capture('liquid-closed')
    control('apply', patch={})
    assert max_delta(closed.crop((0, 80, 600, 600)), capture('liquid-close-repaint').crop((0, 80, 600, 600))) == 0
    print('PASS: Liquid shadow ownership, movement/resize/close cleanup, fullscreen and monitor edges', flush=True)

    metrics = {}
    for preset in ('frosted-glass-clear-edge', 'frosted-liquid-medium'):
        control('apply', profile={'base_preset': preset, 'theme': 'dark'})
        for i in range(4):
            control('apply', patch={'overrides': {'glass_opacity': 1 if i % 2 else 0.9}})
            time.sleep(0.15)
        capture('liquid-audit-' + preset)
        data = control('buffers')['buffers']
        assert data['allocation_failures'] == 0 and data['window_pool']['leased_bytes'] == 0
        assert all(w['owns_shadow'] == (preset == 'frosted-liquid-medium') for w in data['windows'] if w['enabled'])
        metrics[preset] = {'last_frame_bytes': data['last_frame_bytes'], 'retained_bytes': data['retained_bytes'],
                           'cpu_frame_ms_median_recent': statistics.median(data['cpu_frame_ms'][-4:]),
                           'buffers': data['buffers']}
    (build / 'liquid-buffer-audit.json').write_text(json.dumps(metrics, indent=2))
    print('PASS: Liquid/native shadow restoration and buffer/CPU-submission audit', flush=True)
    # Half-sharp targets participate in the same bounded idle pool retirement.
    for client in clients:
        if client.poll() is None:
            client.terminate(); client.wait(timeout=5)
    wait(lambda: not json.loads(ctl('-j', 'clients')))
    time.sleep(6.2)
    idle = control('buffers')['buffers']
    assert idle['window_pool']['bytes'] == 0 and idle['window_pool']['leased_bytes'] == 0
    assert not any('material-backdrop' in b['role'] for b in idle['buffers'])
    print('PASS: Liquid half-resolution targets retire with the idle pool', flush=True)
