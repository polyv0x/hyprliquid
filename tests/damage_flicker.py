"""Replay small split-window commits without intervening screenshot repaints."""
import gzip
import json
from pathlib import Path
import signal
import subprocess
import time


def verify_damage_flicker(ctl, env, log, clients, build, wait, fixtures):
    def control(op, **fields):
        reply = json.loads(ctl('hyprliquid:control', json.dumps({'op': op, **fields})))
        assert reply['ok'], reply
        return reply

    def write(path, value):
        temporary = path.with_suffix('.tmp')
        temporary.write_text(json.dumps(value))
        temporary.replace(path)
        def acknowledged():
            try:
                return json.loads(Path(str(path) + '.ack').read_text()) == value
            except (OSError, ValueError):
                return False
        wait(acknowledged)

    def capture(name, trigger=None):
        path = build / name
        ctl('hyprliquid-test:capture', ('next ' if trigger else '') + str(path))
        if trigger:
            trigger()
        wait(lambda: path.with_suffix('.json').exists())
        metadata = json.loads(path.with_suffix('.json').read_text())
        assert metadata['gl_error'] == 0, metadata
        pixels = path.with_suffix('.raw').read_bytes()
        path.with_suffix('.raw').unlink()
        return pixels, metadata

    ctl('plugin', 'load', str(fixtures / 'capture-test.so'))
    ctl('dismissnotify')
    ctl('eval', 'hl.config({ animations = { enabled = false }, general = { border_size = 3, gaps_in = 8, gaps_out = 12 }, '
        'decoration = { active_opacity = 1, inactive_opacity = 1, dim_inactive = false, '
        'blur = { enabled = true, size = 8, passes = 3 } }, cursor = { no_warps = true, inactive_timeout = 0.1 }, '
        'render = { use_fp16 = 2 } }); '
        'hl.monitor({output="HYPRLIQUID-TEST", mode="3840x2160@160", position="auto", scale=1.25, transform=0, bitdepth=8, cm="srgb"}); '
        'hl.layer_rule({name="flicker-bar", match={namespace="^hyprliquid-vram-test$"}, ignore_alpha=0.5, no_anim=true})')
    control('apply', profile={'base_preset': 'frosted-glass', 'theme': 'dark'})
    bar = subprocess.Popen([str(fixtures / 'layer-test'), '--bar'], env=env | {'GSK_RENDERER': 'cairo', 'GTK_A11Y': 'none'}, stdout=log, stderr=log)
    clients.append(bar)
    wait(lambda: 'hyprliquid-vram-test' in ctl('-j', 'layers'))
    paths = []
    initial = {'root': {'alpha': 70, 'color': 0x183048}, 'child': {'alpha': 170, 'color': 0x305070,
               'patch': 0x507090, 'patch_x': 5, 'patch_y': 5, 'patch_w': 28, 'patch_h': 18},
               'grandchild': {'mapped': True, 'width': 180, 'height': 80, 'x': 40, 'y': 120, 'color': 0x804020}}
    for side in range(2):
        path = build / f'split-{side}.json'
        path.write_text(json.dumps(initial))
        paths.append(path)
        client = subprocess.Popen([str(fixtures / 'subsurface-test'), f'flicker-{side}', str(path)], env=env, stdout=log, stderr=log)
        clients.append(client)
        wait(lambda: len(json.loads(ctl('-j', 'clients'))) == side + 1)
    time.sleep(.4)
    geometry = json.loads(ctl('-j', 'clients'))
    assert all(not window['floating'] for window in geometry), geometry
    (build / 'geometry.json').write_text(json.dumps(geometry, indent=2))
    states = []
    current = [json.loads(json.dumps(initial)) for _ in range(2)]
    bar_phase = 0
    for index in range(24):
        if index % 8 == 3:
            bar_phase ^= 1
            states.append((2, bar_phase))
        side = index % 2
        value = json.loads(json.dumps(current[side]))
        value['child']['patch'] = 0xd05040 if index // 2 % 2 == 0 else 0x40b080
        value['child']['partial'] = True
        if index % 8 == 7:
            value['child']['phase'] = index // 8 % 2
            value['child']['partial'] = False
        if index % 8 == 6:
            value['grandchild']['x'] += 7
        current[side] = value
        states.append((side, value))

    bar_current = 0
    def update(side, value):
        nonlocal bar_current
        if side < 2:
            write(paths[side], value)
        elif value != bar_current:
            bar.send_signal(signal.SIGUSR1)
            bar_current = value
            time.sleep(.08)

    def replay(case, states, count):
        # Generate ground truth with an explicitly rebuilt material and full
        # native frame. The tested replay below requests only next-frame readback;
        # screenshot-triggered repaints must not repair between its commits.
        references = []
        for index, (side, value) in enumerate(states):
            update(side, value)
            control('apply', patch={})
            pixels, metadata = capture(f'{case}-reference-{index}')
            path = build / f'{case}-reference-{index}.raw.gz'
            with gzip.open(path, 'wb') as output:
                output.write(pixels)
            references.append(path)
        for path in paths[:count]:
            write(path, initial)
        update(2, 0)
        control('apply', patch={})
        presented, _ = capture(f'{case}-replay-initial')
        results = []
        for index, (side, value) in enumerate(states):
            pixels, metadata = capture(f'{case}-replay-{index}', lambda: update(side, value))
            composed = bytearray(presented)
            stride = metadata['width'] * metadata['pixel_bytes']
            for x0, y0, x1, y1 in metadata['native_damage']:
                for y in range(y0, y1):
                    lo, hi = y * stride + x0 * metadata['pixel_bytes'], y * stride + x1 * metadata['pixel_bytes']
                    composed[lo:hi] = pixels[lo:hi]
            presented = bytes(composed)
            reference = gzip.open(references[index], 'rb').read()
            mismatches = sum(a != b for a, b in zip(presented, reference))
            results.append({'index': index, 'side': side, 'mismatched_bytes': mismatches, 'damage': metadata['native_damage']})
            (build / f'{case}-flicker.json').write_text(json.dumps(results, indent=2))
            if mismatches:
                for name, content in [('presented', presented), ('expected', reference), ('internal', pixels)]:
                    with gzip.open(build / f'{case}-failed-{index}-{name}.raw.gz', 'wb') as output:
                        output.write(content)
                print(f'FAIL: {case} hover/status update {index}: {mismatches} incorrect bytes', flush=True)
            else:
                print(f'PASS: {case} hover/status update {index} matches full rebuild', flush=True)
        assert not any(item['mismatched_bytes'] for item in results), 'Partial redraw sequence differs from full rebuilds'
        assert any(sum((x1-x0)*(y1-y0) for x0,y0,x1,y1 in item['damage']) < metadata['width']*metadata['height']/10
                   for item in results), 'Replay did not exercise small partial redraws'

    replay('split', states, 2)
    client.terminate()
    client.wait(timeout=5)
    wait(lambda: len(json.loads(ctl('-j', 'clients'))) == 1)
    single = []
    for phase, color in enumerate((0xd05040, 0x40b080)):
        value = json.loads(json.dumps(initial))
        value['child'].update(patch=color, partial=True)
        single.extend(((2, 1 - phase), (0, value)))
    write(paths[0], initial)
    update(2, 0)
    replay('single', single, 1)
    ctl('plugin', 'unload', str(fixtures / 'capture-test.so'))
