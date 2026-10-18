"""Pixel regressions for neighbouring-window refraction in real Hyprland."""
import json
import subprocess
import time


def verify_window_scene(ctl, env, log, clients, build, wait):
    from PIL import Image, ImageChops
    # Remove the earlier floating test scene so these are ordinary adjacent tiles.
    for client in clients:
        if client.poll() is None:
            client.terminate()
            client.wait(timeout=5)
    wait(lambda: not json.loads(ctl('-j', 'clients')))
    ctl('reload')
    ctl('dismissnotify')
    ctl('eval', 'hl.config({ general = { border_size = 0, gaps_in = 8, gaps_out = 20 }, '
        'decoration = { active_opacity = 1, inactive_opacity = 1, dim_inactive = false, shadow = { enabled = false } }, '
        'cursor = { no_warps = true, inactive_timeout = 0.1 } })')
    ctl('eval', 'hl.plugin.hyprliquid.config({ default_preset = "lens" })')

    pattern = r'''
import json, os, shutil, sys, time
from pathlib import Path
control = Path(sys.argv[1])
side = int(sys.argv[2])
previous = None
while True:
    size = shutil.get_terminal_size()
    data = json.loads(control.read_text())
    state = (size, data)
    if state != previous:
        previous = state
        out = '\x1b[?25l\x1b[0m\x1b[2J'
        for row in range(size.lines):
            out += '\x1b[%d;1H' % (row + 1)
            if not data.get('empty') and (data.get('solid') or (row // 4) % 2 == data['phase']):
                out += '\x1b[48;2;%d;%d;%dm' % tuple(data['color']) + ' ' * (size.columns - 1) + '\x1b[0m'
        print(out, end='', flush=True)
    time.sleep(0.05)
'''
    controls = []
    for side, color in enumerate(([220, 40, 20], [20, 200, 70])):
        path = build / f'scene-color-{side}.json'
        path.write_text(json.dumps({'color': color, 'phase': side}))
        controls.append(path)
        clients.append(subprocess.Popen([
            'kitty', '--start-as=normal', '--class', f'hyprliquid-scene-{side}',
            '-o', 'remember_window_size=no', '-o', 'background_opacity=0',
            '-o', 'cursor_blink_interval=0', '-o', 'font_size=16',
            'python3', '-c', pattern, str(path), str(side),
        ], env=env, stdout=log, stderr=log))
        wait(lambda: len(json.loads(ctl('-j', 'clients'))) == side + 1)
    time.sleep(0.5)
    windows = sorted(json.loads(ctl('-j', 'clients')), key=lambda w: tuple(w['at']))
    assert all(not w['floating'] and w['fullscreen'] == 0 for w in windows), windows
    assert windows[0]['at'][0] + windows[0]['size'][0] < windows[1]['at'][0], windows
    (build / 'scene-clients.json').write_text(json.dumps(windows, indent=2))

    def buffers():
        reply = json.loads(ctl('hyprliquid:control', '{"op":"buffers"}'))
        return reply.get('buffers') if reply['ok'] else None

    initial_buffers = buffers()
    if initial_buffers:
        assert initial_buffers['window_pool']['hits'] > 0, initial_buffers
        assert initial_buffers['window_pool']['leased_bytes'] == 0
        scene = [b for b in initial_buffers['buffers'] if b['role'].startswith('scene-')]
        assert len(scene) == 2, scene
        (build / 'scene-buffers-warm.json').write_text(json.dumps(initial_buffers, indent=2))

        # These materials have the same 97-pixel padding and therefore share
        # exact-size samples. Zero blur must not destroy the pooled blur target
        # needed by the next consumer with blur enabled.
        allocations = None
        last_frame = initial_buffers['frame']
        for sigma in (10, 0, 10, 0, 10, 0, 10):
            request = {'op': 'apply', 'profile': {'base_preset': 'lens', 'overrides': {
                'blur_sigma': sigma, 'outer_refraction_amount': 95 if sigma == 0 else 50,
                'inner_refraction_amount': 0, 'aberration_amount': 0, 'edge_bleed_opacity': 0}}}
            assert json.loads(ctl('hyprliquid:control', json.dumps(request)))['ok']
            wait(lambda: buffers()['frame'] > last_frame)
            current = buffers()
            assert all(w['padding'] == 97 for w in current['windows'])
            if allocations is None:
                allocations = current['allocations']
            assert current['allocations'] == allocations, 'Zero blur discarded reusable pooled storage'
            last_frame = current['frame']
        assert json.loads(ctl('hyprliquid:control', '{"op":"reset"}'))['ok']
        print('PASS: alternating zero/nonzero blur reuses matching storage without allocation churn', flush=True)

    def focus(window):
        ctl('dispatch', 'hl.dsp.focus({ window = ' + json.dumps('address:' + window['address']) + ' })')
        if window['floating']:
            # The focus dispatcher does not raise a float. Do both, as a click
            # would, so the regression actually exercises a new draw order.
            ctl('dispatch', 'hl.dsp.window.alter_zorder({ window = ' +
                json.dumps('address:' + window['address']) + ', mode = "top" })')

    def capture(name):
        time.sleep(0.4)
        path = build / f'{name}.png'
        subprocess.run(['grim', str(path)], env=env, check=True, capture_output=True, timeout=10)
        return Image.open(path).convert('RGB')

    def inside(image, window, edge=False):
        scale = json.loads(ctl('-j', 'monitors'))[0]['scale']
        x, y = window['at']
        w, h = window['size']
        if edge:
            box = (x + w - 64, y + 36, x + w - 4, y + h - 36)
        else:
            # Hyprland's nested-launch warning has an animated timer at the top.
            box = (x + 4, y + 36, x + w - 4, y + h - 4)
        return image.crop(tuple(round(v * scale) for v in box))

    focus(windows[0])
    first = capture('scene-focus-left')
    focus(windows[1])
    second = capture('scene-focus-right')
    for window in windows:
        difference = ImageChops.difference(inside(first, window), inside(second, window))
        assert difference.getbbox() is None, f'Glass changed on focus: {window["class"]}, {difference.getbbox()}'
    print('PASS: adjacent tiled panes remain pixel-identical across focus changes', flush=True)

    # The earlier-drawn pane must actually see its later-drawn neighbour's new
    # pixels. Equality alone would also pass if all neighbour optics were removed.
    side = int(windows[1]['class'].rsplit('-', 1)[1])
    controls[side].write_text(json.dumps({'color': [30, 60, 240], 'phase': side}))
    updated = capture('scene-neighbour-repaint')
    difference = ImageChops.difference(inside(second, windows[0], True), inside(updated, windows[0], True))
    changed = sum(max(pixel) > 4 for pixel in difference.getdata())
    assert changed > 100, f'Neighbour repaint did not reach earlier pane: only {changed} changed pixels'
    focus(windows[0])
    again = capture('scene-repaint-focus-left')
    for window in windows:
        assert inside(again, window).tobytes() == inside(updated, window).tobytes(), 'Updated reflection changed on focus'
    print(f'PASS: neighbour repaint updates {changed} pixels in the earlier pane without feedback', flush=True)

    def contribution(value):
        request = {'op': 'apply', 'patch': {'overrides': {'neighbor_contribution': value}}}
        reply = json.loads(ctl('hyprliquid:control', json.dumps(request)))
        assert reply['ok'], reply
        return reply['state']['material']

    allocations = buffers()['allocations'] if initial_buffers else None
    strengths = {}
    material = json.loads(ctl('hyprliquid:control', '{"op":"state"}'))['state']['material']
    for strength in (1, 0.5, 0):
        current = contribution(strength)
        assert current == {**material, 'neighbor_contribution': strength}, 'Contribution changed other material settings'
        focus(windows[0])
        first = capture(f'scene-contribution-{strength}-left')
        focus(windows[1])
        second = capture(f'scene-contribution-{strength}-right')
        for window in windows:
            assert inside(first, window).tobytes() == inside(second, window).tobytes(), \
                f'Contribution {strength} changed on focus'
        strengths[strength] = second
    x, y = windows[0]['at']
    w, h = windows[0]['size']
    center = (x + w // 3, y + h // 3, x + 2 * w // 3, y + 2 * h // 3)
    assert len({frame.crop(center).tobytes() for frame in strengths.values()}) == 1, 'Contribution changed the main frost'
    zero = inside(strengths[0], windows[0], True)
    distance = {s: sum(sum(p) for p in ImageChops.difference(zero, inside(frame, windows[0], True)).getdata())
                for s, frame in strengths.items()}
    assert 0 < distance[0.5] < distance[1], f'Contribution did not weaken the neighbour: {distance}'
    controls[side].write_text(json.dumps({'color': [20, 200, 70], 'phase': side}))
    hidden = capture('scene-contribution-zero-repaint')
    assert inside(hidden, windows[0]).tobytes() == inside(strengths[0], windows[0]).tobytes(), \
        'Zero contribution leaked the neighbour through the native backdrop'
    if initial_buffers:
        assert buffers()['allocations'] == allocations, 'Contribution allocated extra framebuffer storage'
    assert json.loads(ctl('hyprliquid:control', json.dumps({'op': 'apply', 'patch': {
        'reset_fields': ['neighbor_contribution']}})))['ok']
    controls[side].write_text(json.dumps({'color': [30, 60, 240], 'phase': side}))
    print('PASS: neighbour strength 0/0.5/1, unchanged main frost, focus stability and no extra framebuffers', flush=True)

    def falloff(value):
        request = {'op': 'apply', 'patch': {'overrides': {'neighbor_falloff': value}}}
        reply = json.loads(ctl('hyprliquid:control', json.dumps(request)))
        assert reply['ok'], reply

    falloff(0)  # Compare against zero regardless of the shipped default.
    full = capture('scene-falloff-zero')
    falloff_distances = []
    allocations = buffers()['allocations'] if initial_buffers else None
    for rate in (0.25, 0.5, 1):
        falloff(rate)
        focus(windows[0])
        first = capture(f'scene-falloff-{rate}-left')
        focus(windows[1])
        second = capture(f'scene-falloff-{rate}-right')
        for window in windows:
            assert inside(first, window).tobytes() == inside(second, window).tobytes(), f'Falloff {rate} changed on focus'
        assert second.crop(center).tobytes() == full.crop(center).tobytes(), 'Falloff faded the main frost'
        distance = sum(sum(p) for p in ImageChops.difference(
            inside(full, windows[0], True), inside(second, windows[0], True)).getdata())
        falloff_distances.append(distance)
        if initial_buffers:
            current = buffers()
            assert current['window_pool']['leased_bytes'] == 0, 'Falloff retained a sample lease'
            assert current['allocation_failures'] == 0
            assert not any(b['role'].startswith('window-clean') for b in current['buffers']), 'Detail falloff used an extra framebuffer'
            if rate == 0.25:
                (build / 'scene-falloff-buffers.json').write_text(json.dumps(current, indent=2))
            assert current['allocations'] == allocations, 'Falloff adjustment churned framebuffer allocations'
    assert 0 < falloff_distances[0] < falloff_distances[1] < falloff_distances[2], falloff_distances
    contribution(0)
    no_neighbor = capture('scene-falloff-no-neighbor')
    controls[side].write_text(json.dumps({'color': [20, 200, 70], 'phase': side}))
    hidden = capture('scene-falloff-no-neighbor-repaint')
    assert inside(hidden, windows[0]).tobytes() == inside(no_neighbor, windows[0]).tobytes(), 'Falloff leaked a disabled neighbor'
    controls[side].write_text(json.dumps({'color': [30, 60, 240], 'phase': side}))
    contribution(1)
    falloff(0)
    reset = capture('scene-falloff-reset')
    assert inside(reset, windows[0]).tobytes() == inside(full, windows[0]).tobytes(), 'Falloff zero did not restore identical pixels'
    # Exercise detail falloff while tile geometry overlaps too.
    falloff(0.25)
    print('PASS: progressive detail falloff, unchanged main frost, exact reset and no additional storage', flush=True)

    # Splitting the right tile creates a temporary overlap: the old neighbour
    # is still shrinking while the new one opens below it. Neither reflection
    # may disappear from the unchanged left tile during that transition.
    focus(windows[1])
    controls[side].write_text(json.dumps({'color': [30, 60, 240], 'phase': side, 'solid': True}))
    ctl('eval', 'hl.config({ animations = { enabled = true } }); '
        'hl.animation({ leaf = "windows", enabled = true, speed = 30, bezier = "linear" }); '
        'hl.animation({ leaf = "windowsIn", enabled = true, speed = 30, bezier = "linear", style = "popin 92%" }); '
        'hl.animation({ leaf = "fade", enabled = false })')
    path = build / 'scene-color-new.json'
    path.write_text(json.dumps({'color': [20, 200, 70], 'phase': 0, 'solid': True}))
    new_client = subprocess.Popen([
        'kitty', '--start-as=normal', '--class', 'hyprliquid-scene-new',
        '-o', 'remember_window_size=no', '-o', 'background_opacity=0',
        '-o', 'cursor_blink_interval=0', '-o', 'font_size=16',
        'python3', '-c', pattern, str(path), '2',
    ], env=env, stdout=log, stderr=log)
    clients.append(new_client)
    wait(lambda: len(json.loads(ctl('-j', 'clients'))) == 3)
    opening = []
    for frame in range(3):
        pixels = inside(capture(f'scene-split-opening-{frame}'), windows[0], True)
        upper = pixels.crop((0, 0, pixels.width, pixels.height // 2))
        lower = pixels.crop((0, pixels.height // 2, pixels.width, pixels.height))
        blue = sum(b > max(r, g) + 20 and b > 60 for r, g, b in upper.getdata())
        green = sum(g > max(r, b) + 20 and g > 60 for r, g, b in lower.getdata())
        sample = {'blue': blue, 'green': green}
        if initial_buffers:
            sample['boxes'] = [w['pixel_box'] for w in buffers()['windows']]
        opening.append(sample)
    (build / 'scene-split-opening.json').write_text(json.dumps(opening, indent=2))
    if initial_buffers:
        overlaps = []
        for sample in opening:
            right = sorted(sample['boxes'], key=lambda b: b[0])[1:]
            assert len(right) == 2, sample
            a, b = right
            overlaps.append(min(a[1] + a[3], b[1] + b[3]) > max(a[1], b[1]))
        assert any(overlaps), f'Fixture did not exercise overlapping animated tiles: {opening}'
        assert opening[0]['boxes'] != opening[-1]['boxes'], 'Fixture captured settled geometry'
    assert all(p['blue'] > 100 and p['green'] > 100 for p in opening), \
        f'Opening a right-hand split lost neighbour reflections on the left: {opening}'
    time.sleep(3)
    new_client.terminate()
    new_client.wait(timeout=5)
    wait(lambda: len(json.loads(ctl('-j', 'clients'))) == 2)
    closing = inside(capture('scene-split-closing'), windows[0], True)
    assert sum(b > max(r, g) + 20 and b > 60 for r, g, b in closing.getdata()) > 100, \
        'Closing a right-hand split lost the remaining neighbour reflection'
    time.sleep(3)
    ctl('eval', 'hl.config({ animations = { enabled = false } })')
    controls[side].write_text(json.dumps({'color': [30, 60, 240], 'phase': side}))
    falloff(0)
    print('PASS: opening and closing a right-hand split preserve neighbour reflections during animation', flush=True)

    monitor_name = json.loads(ctl('-j', 'monitors'))[0]['name']
    for scale, transform in ((1.25, 0), (1, 1)):
        ctl('eval', 'hl.monitor({ output = ' + json.dumps(monitor_name) +
            f', mode = "1280x800@60", position = "auto", scale = {scale}, transform = {transform} ' + '})')
        time.sleep(0.7)
        windows = sorted(json.loads(ctl('-j', 'clients')), key=lambda w: tuple(w['at']))
        side = int(windows[1]['class'].rsplit('-', 1)[1])
        phase = 0 if transform else side
        for strength, rate in ((0, 0), (0, 1), (0.5, 0), (0.5, 0.5), (1, 0), (1, 0.5)):
            contribution(strength)
            falloff(rate)
            controls[side].write_text(json.dumps({'color': [30, 60, 240], 'phase': phase}))
            name = f'scene-scale-{scale}-transform-{transform}-contribution-{strength}-falloff-{rate}'
            focus(windows[0])
            first = capture(name + '-left')
            focus(windows[1])
            second = capture(name + '-right')
            for window in windows:
                assert inside(first, window).tobytes() == inside(second, window).tobytes(), f'Focus changed optics: {name}'
            controls[side].write_text(json.dumps({'color': [20, 200, 70], 'phase': phase}))
            updated = capture(name + '-repaint')
            difference = ImageChops.difference(inside(second, windows[0]), inside(updated, windows[0]))
            changed = sum(max(pixel) > 4 for pixel in difference.getdata())
            assert (changed == 0 if strength == 0 else changed > 100), f'Wrong neighbour contribution: {name}, {changed}'
        print(f'PASS: focus-stable neighbour contribution and falloff at scale {scale}, transform {transform}', flush=True)

    # Floating neighbours have the same optical relationship when their boxes
    # are separate. Focus changes their stacking order, not their contribution.
    ctl('eval', 'hl.monitor({ output = ' + json.dumps(monitor_name) +
        ', mode = "1280x800@60", position = "auto", scale = 1.25, transform = 0 })')
    time.sleep(0.7)

    def action(window, name, values):
        ctl('dispatch', 'hl.dsp.window.' + name + '({ window = ' +
            json.dumps('address:' + window['address']) + ', ' + values + ' })')

    for index, window in enumerate(windows):
        action(window, 'float', 'action = "enable"')
        action(window, 'resize', 'x = 400, y = 360, relative = false')
        action(window, 'move', f'x = {50 + index * 420}, y = {100 + index * 30}, relative = false')
        side = int(window['class'].rsplit('-', 1)[1])
        controls[side].write_text(json.dumps({'color': [220, 40, 20] if index == 0 else [30, 60, 240], 'phase': index}))
    time.sleep(0.4)
    windows = sorted(json.loads(ctl('-j', 'clients')), key=lambda w: tuple(w['at']))
    assert all(w['floating'] for w in windows)
    assert windows[0]['at'][0] + windows[0]['size'][0] < windows[1]['at'][0]
    (build / 'scene-floating-clients.json').write_text(json.dumps(windows, indent=2))
    floating_frames = {}
    for strength, rate in ((0, 0), (0, 1), (0.5, 0), (1, 0), (1, 0.5)):
        contribution(strength)
        falloff(rate)
        focus(windows[0])
        first = capture(f'scene-floating-{strength}-{rate}-left')
        focus(windows[1])
        second = capture(f'scene-floating-{strength}-{rate}-right')
        for window in windows:
            assert inside(first, window).tobytes() == inside(second, window).tobytes(), \
                f'Floating neighbour changed on focus: contribution {strength}, falloff {rate}'
        floating_frames[strength, rate] = second
    zero = inside(floating_frames[0, 0], windows[0], True)
    distance = {key: sum(sum(p) for p in ImageChops.difference(zero, inside(frame, windows[0], True)).getdata())
                for key, frame in floating_frames.items()}
    assert 0 < distance[0.5, 0] < distance[1, 0], f'Floating contribution slider had no effect: {distance}'
    assert inside(floating_frames[1, 0.5], windows[0], True).tobytes() != inside(
        floating_frames[1, 0], windows[0], True).tobytes(), 'Floating falloff slider had no effect'
    print('PASS: adjacent floating panes keep both neighbor controls and identical pixels across focus changes', flush=True)

    # An opaque window beneath a transparent receiver must block the wallpaper
    # even along refracted edges outside the receiver's bounds. Neither slider
    # may fade this true backdrop away. Exercise rotated framebuffer coordinates
    # as well as the fractional scale used on DP-1.
    underlay, receiver = windows
    underlay_side = int(underlay['class'].rsplit('-', 1)[1])
    receiver_side = int(receiver['class'].rsplit('-', 1)[1])
    for scale, transform in ((1.25, 0), (1, 1)):
        ctl('eval', 'hl.monitor({ output = ' + json.dumps(monitor_name) +
            f', mode = "1280x800@60", position = "auto", scale = {scale}, transform = {transform} ' + '})')
        time.sleep(0.7)
        action(underlay, 'resize', 'x = 700, y = 540, relative = false')
        action(underlay, 'move', 'x = 50, y = 60, relative = false')
        action(receiver, 'resize', 'x = 400, y = 300, relative = false')
        action(receiver, 'move', 'x = 300, y = 180, relative = false')
        controls[underlay_side].write_text(json.dumps({'color': [30, 60, 240], 'phase': 0, 'solid': True}))
        controls[receiver_side].write_text(json.dumps({'color': [0, 0, 0], 'phase': 0, 'empty': True}))
        focus(receiver)
        protected = tuple(round(v * scale) for v in (304, 260, 324, 400))
        references = {}
        for strength, rate in ((1, 0), (1, 1), (0.5, 1), (0, 0), (0, 1)):
            contribution(strength)
            falloff(rate)
            edge = capture(f'scene-underlay-{scale}-{transform}-{strength}-{rate}').crop(protected)
            if strength == 1:
                references[rate] = edge
                assert all(b > max(r, g) + 50 for r, g, b in edge.getdata()), 'Opaque underlay fixture missing'
            else:
                # Falloff can soften nearby rim/shadow detail; contribution
                # must preserve this same true backdrop at either setting.
                delta = max(max(pixel) for pixel in ImageChops.difference(references[rate], edge).getdata())
                assert delta <= 1, f'Neighbor controls exposed wallpaper through underlay: {scale}, {transform}, {strength}, {rate}, {delta}'
        controls[underlay_side].write_text(json.dumps({'color': [20, 200, 70], 'phase': 0, 'solid': True}))
        repainted = capture(f'scene-underlay-{scale}-{transform}-repaint').crop(protected)
        assert all(g > max(r, b) + 50 for r, g, b in repainted.getdata()), 'Underlay refraction did not repaint'
        if transform == 0:
            state = json.loads(ctl('hyprliquid:control', '{"op":"state"}'))['state']
            sigma = state['material']['blur_sigma']
            before_frame = buffers()['frame'] if initial_buffers else None
            assert json.loads(ctl('hyprliquid:control', json.dumps({'op': 'apply', 'patch': {
                'overrides': {'blur_sigma': 0}}})))['ok']
            sharp = capture('scene-underlay-zero-blur').crop(protected)
            assert all(g > max(r, b) + 50 for r, g, b in sharp.getdata()), 'Zero-blur underlay faded away'
            if initial_buffers:
                current = buffers()
                assert not any(b['role'].startswith('window-clean') and b['last_used_frame'] > before_frame
                               for b in current['buffers']), 'Zero-blur falloff used an extra framebuffer'
            assert json.loads(ctl('hyprliquid:control', json.dumps({'op': 'apply', 'patch': {
                'overrides': {'blur_sigma': sigma}}})))['ok']
        print(f'PASS: opaque underlay blocks wallpaper at every neighbor setting, scale {scale}, transform {transform}', flush=True)
    ctl('eval', 'hl.monitor({ output = ' + json.dumps(monitor_name) +
        ', mode = "1280x800@60", position = "auto", scale = 1.25, transform = 0 })')
    time.sleep(0.7)
    # Track a uniform patch of an opaque lower window while dragging
    # the glass left. The sampled patch crosses from neighbor capture into the
    # native backdrop, then enters the frost. Its brightness must not dip and
    # recover. Keep the receiving pixels outside the lower window so this also
    # catches the handoff that happens independently of underlay protection.
    original_panel = json.loads(ctl('hyprliquid:control', '{"op":"state"}'))['state']['panel']
    neutral = {
        'neighbor_contribution': 1, 'neighbor_falloff': 1, 'blur_sigma': 8,
        'outer_refraction_amount': 100, 'inner_refraction_amount': 100,
        'outer_refraction_inv_height': 0.001, 'inner_refraction_inv_height': 0.001,
        'aberration_amount': 0, 'edge_bleed_opacity': 0, 'face_opacity': 0,
        'holding_tone_opacity': 0, 'rim_opacity': 0, 'shadow_amount': 0,
        'blur_alpha0': 0.2, 'blur_alpha3': 0.2, 'refraction_opacity': 1,
    }
    assert json.loads(ctl('hyprliquid:control', json.dumps({'op': 'apply', 'patch': {'overrides': neutral}})))['ok']
    action(underlay, 'resize', 'x = 700, y = 540, relative = false')
    action(underlay, 'move', 'x = 50, y = 60, relative = false')
    action(receiver, 'resize', 'x = 400, y = 500, relative = false')
    controls[underlay_side].write_text(json.dumps({'color': [30, 60, 240], 'phase': 0, 'solid': True}))
    controls[receiver_side].write_text(json.dumps({'color': [0, 0, 0], 'phase': 0, 'empty': True}))
    focus(receiver)
    patch = tuple(round(v * 1.25) for v in (770, 340, 774, 360))
    drag_samples = []
    reference = None
    for left in (766, 758, 750, 742, 730, 718, 706, 694, 682, 674, 670, 666, 658, 650):
        action(receiver, 'move', f'x = {left}, y = 100, relative = false')
        frame = capture(f'scene-drag-handoff-{left}').crop(patch)
        if reference is None:
            reference = frame
            assert all(b > max(r, g) + 100 for r, g, b in frame.getdata()), 'Drag fixture missed the lower window'
        delta = max(max(pixel) for pixel in ImageChops.difference(reference, frame).getdata())
        drag_samples.append({'left': left, 'delta': delta, 'pixel': frame.getpixel((2, 10))})
    (build / 'scene-drag-handoff.json').write_text(json.dumps(drag_samples, indent=2))
    assert max(sample['delta'] for sample in drag_samples) <= 1, f'Reflection faded out and back in during drag: {drag_samples}'
    assert json.loads(ctl('hyprliquid:control', json.dumps({'op': 'apply', 'profile': original_panel})))['ok']
    print('PASS: dragging glass through neighbor/native handoff keeps reflected brightness constant', flush=True)

    for index, window in enumerate(windows):
        action(window, 'resize', 'x = 400, y = 360, relative = false')
        action(window, 'move', f'x = {50 + index * 420}, y = {100 + index * 30}, relative = false')
        side = int(window['class'].rsplit('-', 1)[1])
        controls[side].write_text(json.dumps({'color': [220, 40, 20] if index == 0 else [30, 60, 240], 'phase': index}))
    time.sleep(0.4)
    windows = sorted(json.loads(ctl('-j', 'clients')), key=lambda w: tuple(w['at']))

    # Cross the contact boundary and then overlap deeply. Both controls must
    # keep affecting the exposed edge of whichever pane is on top; actual
    # occlusion is allowed to change when the windows are raised.
    overlap_strength = {}
    for right_x in (451, 450, 449, 350):
        action(windows[1], 'move', f'x = {right_x}, y = 130, relative = false')
        windows = sorted(json.loads(ctl('-j', 'clients')), key=lambda w: tuple(w['at']))
        frames = {}
        for strength, rate in ((0, 0), (0.5, 0), (1, 0), (1, 0.5)):
            contribution(strength)
            falloff(rate)
            for index, window in enumerate(windows):
                focus(window)
                frame = capture(f'scene-floating-contact-{right_x}-{strength}-{rate}-{index}')
                x, y = window['at']
                w, h = window['size']
                edge_x = x + w - 64 if index == 0 else x + 4
                edge = (edge_x, y + 36, edge_x + 60, y + h - 36)
                frames[strength, rate, index] = frame.crop(tuple(round(v * 1.25) for v in edge))
        for index in range(2):
            zero = frames[0, 0, index]
            distance = {(strength, rate): sum(sum(p) for p in ImageChops.difference(zero, frame).getdata())
                        for (strength, rate, side), frame in frames.items() if side == index}
            assert 0 < distance[0.5, 0] < distance[1, 0], \
                f'Neighbor contribution stopped at contact/overlap: {right_x}, {index}, {distance}'
            assert frames[1, 0.5, index].tobytes() != frames[1, 0, index].tobytes(), \
                f'Neighbor falloff stopped at contact/overlap: {right_x}, {index}'
            overlap_strength[right_x, index] = distance[1, 0]
    for index in range(2):
        before, after = overlap_strength[451, index], overlap_strength[449, index]
        assert 0.7 < after / before < 1.3, f'Neighbor contribution snapped at first overlap: {overlap_strength}'
    (build / 'scene-floating-overlap-strength.json').write_text(json.dumps(
        {f'{x},{index}': value for (x, index), value in overlap_strength.items()}, indent=2))
    print('PASS: both neighbor controls remain progressive through contact and overlap in either stacking order', flush=True)

    # Actual overlap must retain native occlusion: the newly focused opaque
    # client must cover the other, regardless of the neighbour sliders.
    for index, window in enumerate(windows):
        side = int(window['class'].rsplit('-', 1)[1])
        controls[side].write_text(json.dumps({'color': [220, 40, 20] if index == 0 else [30, 60, 240], 'phase': index, 'solid': True}))
    action(windows[1], 'move', 'x = 350, y = 130, relative = false')
    # Sample opaque terminal cells inside both panes, away from the transparent
    # padding where the lower window should remain visible through the glass.
    overlap = tuple(round(v * 1.25) for v in (385, 190, 400, 240))
    for index, window in enumerate(windows):
        focus(window)
        pixels = list(capture(f'scene-floating-overlap-{index}').crop(overlap).getdata())
        channel = 0 if index == 0 else 2
        assert all(p[channel] > max(p[c] for c in range(3) if c != channel) + 50 for p in pixels), \
            'Overlapping floating panes lost native stacking'
    action(windows[1], 'move', 'x = 470, y = 130, relative = false')
    print('PASS: overlapping floating panes preserve native focus-dependent occlusion', flush=True)

    # A float overlapping one tile must keep the tiled scene and reflect above
    # the tile, even when lowering it moves it before tiles in WindowState.
    for window in windows:
        action(window, 'float', 'action = "disable"')
    time.sleep(0.4)
    windows = sorted(json.loads(ctl('-j', 'clients')), key=lambda w: tuple(w['at']))
    for index, window in enumerate(windows):
        side = int(window['class'].rsplit('-', 1)[1])
        controls[side].write_text(json.dumps({'color': [220, 40, 20] if index == 0 else [30, 60, 240], 'phase': index, 'solid': True}))
    path = build / 'scene-color-occluder.json'
    path.write_text(json.dumps({'color': [20, 200, 70], 'phase': 0, 'solid': True}))
    occluder = subprocess.Popen([
        'kitty', '--start-as=normal', '--class', 'hyprliquid-scene-occluder',
        '-o', 'remember_window_size=no', '-o', 'background_opacity=0',
        '-o', 'cursor_blink_interval=0', '-o', 'font_size=16',
        'python3', '-c', pattern, str(path), '2',
    ], env=env, stdout=log, stderr=log)
    clients.append(occluder)
    wait(lambda: len(json.loads(ctl('-j', 'clients'))) == 3)
    floating = next(w for w in json.loads(ctl('-j', 'clients')) if w['pid'] == occluder.pid)
    action(floating, 'float', 'action = "enable"')
    action(floating, 'resize', 'x = 160, y = 160, relative = false')
    action(floating, 'move', 'x = 530, y = 250, relative = false')
    action(floating, 'alter_zorder', 'mode = "bottom"')
    time.sleep(0.4)
    windows = sorted([w for w in json.loads(ctl('-j', 'clients')) if not w['floating']], key=lambda w: tuple(w['at']))
    contribution(1)
    falloff(0)
    focus(windows[0])
    first = capture('scene-tiled-with-float-left')
    focus(windows[1])
    second = capture('scene-tiled-with-float-right')
    assert inside(first, windows[0]).tobytes() == inside(second, windows[0]).tobytes(), 'Overlapping float disabled tiled capture'
    edge = inside(second, windows[0], True)
    assert sum(b > max(r, g) + 20 and b > 60 for r, g, b in edge.getdata()) > 100, 'Overlapping float removed tiled neighbour'
    assert sum(g > max(r, b) + 20 and g > 60 for r, g, b in edge.getdata()) > 100, 'Overlapping float missing from tiled reflection'
    action(floating, 'alter_zorder', 'mode = "top"')
    raised = capture('scene-tiled-with-float-raised')
    assert inside(second, windows[0]).tobytes() == inside(raised, windows[0]).tobytes(), 'Float reflected below a tile after lowering'
    occluder.terminate()
    occluder.wait(timeout=5)
    wait(lambda: len(json.loads(ctl('-j', 'clients'))) == 2)
    print('PASS: overlapping floats reflect above tiles independent of focus and raise/lower ordering', flush=True)

    # Closing the neighbour must not leave its old capture behind. Repeated
    # captures of the remaining pane should settle to the same pixels.
    side = int(windows[0]['class'].rsplit('-', 1)[1])
    controls[side].write_text(json.dumps({'color': [220, 40, 20], 'phase': 0}))
    closing = windows[1]
    client = next(p for p in clients if p.poll() is None and p.pid == closing['pid'])
    client.terminate()
    client.wait(timeout=5)
    wait(lambda: len(json.loads(ctl('-j', 'clients'))) == 1)
    settled = capture('scene-neighbour-closed')
    stable = capture('scene-after-close-idle')
    remaining = json.loads(ctl('-j', 'clients'))[0]
    assert inside(settled, remaining).tobytes() == inside(stable, remaining).tobytes(), 'Stale scene changed after neighbour closed'
    assert not any(max(pixel[1:]) > pixel[0] + 20 and max(pixel[1:]) > 60
                   for pixel in inside(stable, remaining).getdata()), 'Closed neighbour left coloured pixels in the remaining pane'
    print('PASS: neighbour close discards the scene and settles without stale-frame feedback', flush=True)
    if initial_buffers:
        time.sleep(3.2)
        retired = buffers()
        assert not any(b['role'].startswith('scene-') for b in retired['buffers']), retired
        idle_frame = retired['frame']
        time.sleep(6.2)
        idle = buffers()
        assert idle['window_pool']['bytes'] == 0, idle
        assert idle['frame'] == idle_frame, 'Storage cleanup scheduled idle redraws'
        assert idle['allocation_failures'] == 0
        (build / 'scene-buffers-idle.json').write_text(json.dumps(idle, indent=2))
        print('PASS: scene retirement, idle pool eviction and no cleanup redraws', flush=True)
