"""Pixel regressions for neighbouring tiled-window refraction in real Hyprland."""
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
            if (row // 4) % 2 == data['phase']:
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

    monitor_name = json.loads(ctl('-j', 'monitors'))[0]['name']
    for scale, transform in ((1.25, 0), (1, 1)):
        ctl('eval', 'hl.monitor({ output = ' + json.dumps(monitor_name) +
            f', mode = "1280x800@60", position = "auto", scale = {scale}, transform = {transform} ' + '})')
        time.sleep(0.7)
        windows = sorted(json.loads(ctl('-j', 'clients')), key=lambda w: tuple(w['at']))
        side = int(windows[1]['class'].rsplit('-', 1)[1])
        phase = 0 if transform else side
        controls[side].write_text(json.dumps({'color': [30, 60, 240], 'phase': phase}))
        focus(windows[0])
        first = capture(f'scene-scale-{scale}-transform-{transform}-left')
        focus(windows[1])
        second = capture(f'scene-scale-{scale}-transform-{transform}-right')
        for window in windows:
            assert inside(first, window).tobytes() == inside(second, window).tobytes(), f'Focus changed optics at scale {scale}, transform {transform}'
        controls[side].write_text(json.dumps({'color': [20, 200, 70], 'phase': phase}))
        updated = capture(f'scene-scale-{scale}-transform-{transform}-repaint')
        difference = ImageChops.difference(inside(second, windows[0]), inside(updated, windows[0]))
        assert sum(max(pixel) > 4 for pixel in difference.getdata()) > 100, f'Neighbour missing at scale {scale}, transform {transform}'
        print(f'PASS: live, focus-stable neighbour optics at scale {scale}, transform {transform}', flush=True)

    # Closing the neighbour must not leave its old capture behind. Repeated
    # captures of the remaining resized pane should settle to the same pixels.
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
