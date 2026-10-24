"""Pointer-driven highlights in a disposable compositor, including real pixels."""
import json
import math
import subprocess
import time


def verify_hover_angle(ctl, env, log, clients, build, wait, plugin):
    from PIL import Image, ImageChops
    for client in clients:
        if client.poll() is None:
            client.terminate()
            client.wait(timeout=5)
    wait(lambda: not json.loads(ctl('-j', 'clients')))
    ctl('reload')
    ctl('dismissnotify')
    ctl('eval', 'hl.config({ general = { border_size = 0 }, '
        'decoration = { active_opacity = 1, inactive_opacity = 1, dim_inactive = false }, '
        'cursor = { no_warps = true, inactive_timeout = 0.1 } })')
    primary = json.loads(ctl('-j', 'monitors'))[0]['name']
    ctl('eval', 'hl.monitor({ output = ' + json.dumps(primary) + ', mode = "1280x800@60", position = "0x0", scale = 1, transform = 0 })')

    def control(op='buffers', **fields):
        reply = json.loads(ctl('hyprliquid:control', json.dumps({'op': op, **fields})))
        assert reply['ok'], reply
        return reply

    def move(x, y):
        ctl('dispatch', f'hl.dsp.cursor.move({{ x = {x}, y = {y} }})')

    def monitor(name=primary):
        return next(m for m in control()['buffers']['monitors'] if m['name'] == name)

    def settled(angle, name=primary):
        current = monitor(name)
        return not current['hover_angle_animating'] and abs(math.remainder(current['hover_angle_degrees'] - angle, 360)) < 0.1

    def capture(name):
        time.sleep(0.25)
        path = build / (name + '.png')
        subprocess.run(['grim', '-o', primary, str(path)], env=env, check=True, capture_output=True, timeout=10)
        return Image.open(path).convert('RGB')

    def sample_windows():
        return {w['owner']: w['rendered_display_angle_degrees'] for w in control()['buffers']['windows'] if w['enabled']}

    profile = {'base_preset': 'frosted-liquid-medium', 'theme': 'dark',
               'overrides': {'blur_sigma': 0, 'display_angle_degrees': 15}}
    control('apply', profile=profile)
    for index in range(2):
        clients.append(subprocess.Popen(['kitty', '--start-as=normal', '--class', f'hover-angle-{index}',
            '-o', 'remember_window_size=no', '-o', 'background_opacity=0', '-o', 'cursor_blink_interval=0',
            'sh', '-c', 'printf "\033[?25l\033[2J"; sleep 120'], env=env, stdout=log, stderr=log))
        wait(lambda: len(json.loads(ctl('-j', 'clients'))) == index + 1)
    move(640, 40)
    baseline = capture('hover-disabled-baseline')
    assert len(sample_windows()) == 2
    ctl('eval', 'hl.curve("hover-ease", { type = "bezier", points = {{0,0},{1,1}} }); '
        'hl.config({ animations = { enabled = true } }); '
        'hl.animation({ leaf = "windowsMove", enabled = true, speed = 6, bezier = "hover-ease" })')
    state = control('apply', patch={'animate_hover_angle': True})['state']
    move(1200, 400)
    samples = []
    deadline = time.monotonic() + 3
    while time.monotonic() < deadline:
        values = sample_windows()
        samples.append(values)
        assert max(values.values()) - min(values.values()) < 0.001, values
        if len(samples) > 3 and settled(90):
            break
        time.sleep(0.015)
    assert any(20 < min(s.values()) < 95 for s in samples), 'Hover did not interpolate'
    wait(lambda: all(abs(v - 105) < 0.1 for v in sample_windows().values()))
    assert control('state')['state'] == state, 'Pointer motion changed runtime ownership'
    assert ImageChops.difference(baseline, capture('hover-right')).getbbox(), 'Hover changed diagnostics but not pixels'
    # Stopping motion must finish the animation. The center dead zone retains
    # the last direction instead of producing flicker around an undefined angle.
    move(641, 401)
    time.sleep(0.15)
    assert settled(90)
    time.sleep(0.25)
    idle_frame = control()['buffers']['frame']
    time.sleep(0.3)
    assert control()['buffers']['frame'] - idle_frame <= 1, 'Settled hover kept scheduling frames'

    move(656, 770)
    bottom_right = math.degrees(math.atan2(16, -370))
    wait(lambda: settled(bottom_right))
    move(624, 770)
    seam = []
    deadline = time.monotonic() + 2
    while time.monotonic() < deadline:
        value = monitor()['hover_angle_degrees']
        seam.append(value)
        assert abs(math.remainder(value - 180, 360)) < 5, seam
        if settled(-bottom_right):
            break
        time.sleep(0.015)
    assert settled(-bottom_right), 'Angle did not finish across the +/-180 seam'
    control('apply', patch={'animate_hover_angle': False})
    assert ImageChops.difference(baseline, capture('hover-disabled-restored')).getbbox() is None, 'Disabling hover did not restore exact pixels'
    assert settled(0)
    print('PASS: shared monitor hover, native easing, seam/center continuity, stable ownership and exact disabled pixels', flush=True)

    # The monitor heading remains independent of each window's resize sweep.
    move(1200, 400)
    control('apply', patch={'animate_hover_angle': True, 'animate_display_angle': True})
    wait(lambda: settled(90))
    window = sorted(json.loads(ctl('-j', 'clients')), key=lambda w: w['at'])[0]
    ctl('dispatch', 'hl.dsp.window.resize({ window = ' + json.dumps('address:' + window['address']) + ', x = 100, y = 0, relative = true })')
    resize = []
    deadline = time.monotonic() + 2
    while time.monotonic() < deadline:
        values = sample_windows()
        resize.append(values)
        assert all(89.9 <= v <= 105.1 for v in values.values()), values
        time.sleep(0.015)
    assert all(max(s[owner] for s in resize) > 103 for owner in resize[0]), resize
    assert all(abs(v - 90) < 0.1 for v in resize[-1].values())
    control('apply', patch={'animate_display_angle': False})

    ctl('eval', 'hl.curve("hover-spring", { type = "spring", mass = 1, stiffness = 210, dampening = 25 }); '
        'hl.animation({ leaf = "windowsMove", enabled = true, speed = 5, spring = "hover-spring" })')
    move(80, 400)
    wait(lambda: settled(-90))
    wait(lambda: all(abs(v + 75) < 0.1 for v in sample_windows().values()))
    # Native animation disablement must also apply to the shared hover value.
    ctl('eval', 'hl.config({ animations = { enabled = false } })')
    move(1200, 400)
    wait(lambda: settled(90))
    print('PASS: hover plus adjacent-window resize sweeps, spring curves and disabled native animations', flush=True)

    secondary = 'HYPRLIQUID-HOVER'
    ctl('output', 'create', 'headless', secondary)
    ctl('eval', 'hl.monitor({ output = "' + secondary + '", mode = "800x600@60", position = "-640x0", scale = 1.25, transform = 0 })')
    wait(lambda: len(json.loads(ctl('-j', 'monitors'))) == 2)
    move(-40, 240)
    wait(lambda: settled(90, secondary) and settled(0))
    wait(lambda: all(abs(v - 15) < 0.1 for v in sample_windows().values()))
    workspace = str(window['workspace']['id'])
    ctl('dispatch', 'hl.dsp.workspace.move({ workspace = ' + json.dumps(workspace) + ', monitor = "' + secondary + '" })')
    wait(lambda: all(w['monitor'] == monitor(secondary)['id'] for w in json.loads(ctl('-j', 'clients'))))
    ctl('dispatch', 'hl.dsp.focus({ window = ' + json.dumps('address:' + window['address']) + ' })')
    # Moving a workspace may restore its saved cursor position independently
    # of no_warps. Establish the intended pointer position on the new output.
    move(-40, 240)
    wait(lambda: settled(90, secondary))
    (build / 'hover-monitor-move.json').write_text(json.dumps(control(), indent=2))
    wait(lambda: all(abs(v - 105) < 0.1 for v in sample_windows().values()))
    ctl('eval', 'hl.config({ animations = { enabled = true } })')
    move(-600, 240)
    wait(lambda: monitor(secondary)['hover_angle_animating'])
    ctl('output', 'remove', secondary)
    wait(lambda: len(control()['buffers']['monitors']) == 1)
    print('PASS: monitor crossing, negative desktop coordinates, fractional scale and removal during hover animation', flush=True)

    # Unloading during an active native animation must remove every callback.
    move(640, 40)
    wait(lambda: settled(0))
    move(1200, 400)
    wait(lambda: monitor()['hover_angle_animating'])
    epoch = control('state')['state']['epoch']
    ctl('plugin', 'unload', str(plugin))
    ctl('plugin', 'load', str(plugin))
    current = control('state')['state']
    assert current['epoch'] != epoch and not current['animate_hover_angle'] and not current['active']
    time.sleep(0.7)
    assert not control()['buffers']['allocation_failures']
    (build / 'hover-angle-animation.json').write_text(json.dumps({'move': samples, 'seam': seam, 'resize': resize}, indent=2))
    print('PASS: unload/reload while hover animation is active', flush=True)
